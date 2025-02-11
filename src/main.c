#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "wokwi-api.h"

#define min(a, b)           \
  ({                        \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a < _b ? _a : _b;      \
  })

typedef enum {
  MODE_COMMAND,
  MODE_DATA,
} chip_mode_t;

typedef struct {
  pin_t cs_pin;
  pin_t dc_pin;
  pin_t rst_pin;
  spi_dev_t spi;
  uint8_t spi_buffer[1024];

  /* Framebuffer state */
  buffer_t framebuffer;
  uint32_t width;
  uint32_t height;
  uint32_t radius;

  /* Command state machine */
  chip_mode_t mode;
  uint8_t command_code;
  uint8_t command_size;
  uint8_t command_index;
  uint8_t command_buf[16];
  bool ram_write;

  // Memory and addressing settings
  uint32_t active_column;
  uint32_t active_page;
  uint32_t column_start;
  uint32_t column_end;
  uint32_t page_start;
  uint32_t page_end;
  uint32_t scanning_direction;
} chip_state_t;

/* Chip command codes */
#define CMD_NOP (0x00)  // No Operation
#define CMD_SWRESET (0x01)
#define CMD_SLPIN (0x10)    // Sleep In
#define CMD_SLPOUT (0x11)   // Sleep Out
#define CMD_INVOFF (0x20)   // Display Inversion Off
#define CMD_INVON (0x21)    // Display Inversion On
#define CMD_DISPOFF (0x28)  // Display Inversion On
#define CMD_DISPON (0x29)   // Display Inversion On
#define CMD_CASET (0x2a)    // Column Address Set
#define CMD_PASET (0x2b)    // Page Address Set
#define CMD_RAMWR (0x2c)    // Memory Write
#define CMD_MADCTL (0x36)   // Memory Access Control
#define CMD_COLMOD (0x3a)   // Set 16-bit pixel format
#define CMD_FRMCTR1 (0xb1)  // Frame rate control 1, use by default
#define CMD_FRMCTR2 (0xb2)  // Frame Rate Control (In Idle mode / 8-colors)
#define CMD_FRMCTR3 \
  (0xb3)                   // Frame Rate Control (In Partial mode / full colors)
#define CMD_INVCTR (0xb4)  // Display inversion, use by default
#define CMD_DISSET5 (0xb6)
#define CMD_PWCTR1 (0xc0)  // Power control 1
#define CMD_PWCTR2 (0xc1)
#define CMD_PWCTR3 (0xc2)   // Power control 3
#define CMD_PWCTR4 (0xc3)   // Power Control 4 (in Idle mode/ 8-colors)
#define CMD_PWCTR5 (0xc4)   // Power Control 5 (in Partial mode/ full-colors)
#define CMD_VMCTR (0xc5)    // VCom control 1
#define CMD_GMCTRP1 (0xe0)  // positive gamma correction
#define CMD_GMCTRN1 (0xe1)  // negative gamma correction

/* Scanning direction bits */
#define SCAN_MY (0b10000000)
#define SCAN_MX (0b01000000)
#define SCAN_MV (0b00100000)

static void chip_pin_change(void *user_data, pin_t pin, uint32_t value);
static void chip_spi_done(void *user_data, uint8_t *buffer, uint32_t count);

void chip_reset(chip_state_t *chip) {
  chip->ram_write = false;
  chip->active_column = 0;
  chip->active_page = 0;
  chip->column_start = 0;
  chip->column_end = 239;
  chip->page_start = 0;
  chip->page_end = 239;
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));

  const pin_watch_config_t watch_config = {
      .edge = BOTH,
      .pin_change = chip_pin_change,
      .user_data = chip,
  };

  chip->cs_pin = pin_init("CS", INPUT_PULLUP);
  pin_watch(chip->cs_pin, &watch_config);

  chip->dc_pin = pin_init("DC", INPUT);
  pin_watch(chip->dc_pin, &watch_config);

  chip->rst_pin = pin_init("RST", INPUT_PULLUP);
  pin_watch(chip->rst_pin, &watch_config);

  const spi_config_t spi_config = {
      .sck = pin_init("CLK", INPUT),
      .mosi = pin_init("DIN", INPUT),
      .miso = NO_PIN,
      .done = chip_spi_done,
      .user_data = chip,
  };
  chip->spi = spi_init(&spi_config);

  chip->framebuffer = framebuffer_init(&chip->width, &chip->height);
  chip->radius = min(chip->width, chip->height) / 2;

  chip_reset(chip);

  printf("GC9A01 Driver Chip initialized!\n");
}

/* Converts a 16-bit RGB565 (5 bits for red, 6 for green, 5 for blue) into
 * 32-bit RGBA (8-bit per channel) */
uint32_t rgb565_to_rgba(uint16_t value) {
  return 0xff000000                  // Alpha
         | ((value & 0x001f) << 19)  // Blue
         | ((value & 0x07e0) << 5)   // Green
         | ((value & 0xf800) >> 8);  // Red
}

void chip_pin_change(void *user_data, pin_t pin, uint32_t value) {
  chip_state_t *chip = (chip_state_t *)user_data;

  // Handle CS pin logic
  if (pin == chip->cs_pin) {
    if (value == LOW) {
      chip->command_size = 0;
      chip->command_index = 0;
      spi_start(chip->spi, chip->spi_buffer, sizeof(chip->spi_buffer));
    } else {
      spi_stop(chip->spi);
    }
  }

  // Handle DC pin logic
  if (pin == chip->dc_pin && chip->mode != value) {
    spi_stop(chip->spi);  // Process remaining data in SPI buffer
    chip->mode = value;
    if (pin_read(chip->cs_pin) == LOW) {
      spi_start(chip->spi, chip->spi_buffer, sizeof(chip->spi_buffer));
    }
  }

  if (pin == chip->rst_pin && value == LOW) {
    spi_stop(chip->spi);  // Process remaining data in SPI buffer
    chip_reset(chip);
  }
}

int command_args_size(uint8_t command_code) {
  switch (command_code) {
    case CMD_MADCTL:
    case CMD_PWCTR2:
    case CMD_INVCTR:
    case CMD_VMCTR:
    case CMD_COLMOD:
      return 1;
    case CMD_PWCTR3:
    case CMD_PWCTR4:
    case CMD_PWCTR5:
    case CMD_DISSET5:
      return 2;
    case CMD_FRMCTR1:
    case CMD_FRMCTR2:
    case CMD_PWCTR1:
      return 3;
    case CMD_CASET:
    case CMD_PASET:
      return 4;
    case CMD_FRMCTR3:
      return 6;
    case CMD_GMCTRP1:
    case CMD_GMCTRN1:
      return 16;
    default:
      return 0;
  }
}

void execute_command(chip_state_t *chip) {
  switch (chip->command_code) {
    case CMD_NOP:
      break;

    case CMD_SLPIN:
    case CMD_DISPOFF:
      // Not implemented.
      break;

    case CMD_SLPOUT:
    case CMD_DISPON:
      // Not implemented.
      break;

    case CMD_INVOFF:
    case CMD_INVON:
      // Not implemented.
      break;

    case CMD_RAMWR:
      chip->ram_write = true;
      break;

    case CMD_MADCTL:
      chip->scanning_direction = chip->command_buf[0] & 0xe0;
      break;

    case CMD_CASET:
    case CMD_PASET: {
      uint16_t arg0 = (chip->command_buf[0] << 8) | chip->command_buf[1];
      uint16_t arg2 = (chip->command_buf[2] << 8) | chip->command_buf[3];
      bool set_page = chip->command_code == CMD_PASET;
      if ((chip->scanning_direction & SCAN_MV) ? !set_page : set_page) {
        chip->active_page = arg0;
        chip->page_start = arg0;
        chip->page_end = arg2;
        if (chip->scanning_direction & SCAN_MY) {
          chip->active_page -= 32;
          chip->page_start -= 32;
          chip->page_end -= 32;
        }
      } else {
        chip->active_column = arg0;
        chip->column_start = arg0;
        chip->column_end = arg2;
      }
      break;
    }

    case CMD_PWCTR1:
    case CMD_SWRESET:
    case CMD_COLMOD:
    case CMD_VMCTR:
      // Not implemented.
      break;

    default:
      printf("Warning: unknown command 0x%02x\n", chip->command_code);
      break;
  }
}

void process_command(chip_state_t *chip, uint8_t *buffer,
                     uint32_t buffer_size) {
  chip->ram_write = false;
  for (int i = 0; i < buffer_size; i++) {
    chip->command_code = buffer[i];
    chip->command_size = command_args_size(chip->command_code);
    chip->command_index = 0;
    if (!chip->command_size) {
      execute_command(chip);
    }
  }
}

void process_command_args(chip_state_t *chip, uint8_t *buffer,
                          uint32_t buffer_size) {
  for (int i = 0; i < buffer_size; i++) {
    if (chip->command_index < chip->command_size) {
      chip->command_buf[chip->command_index++] = buffer[i];
      if (chip->command_size == chip->command_index) {
        execute_command(chip);
      }
    }
  }
}

void process_data(chip_state_t *chip, const uint16_t *buffer16,
                  uint32_t buffer_size) {
  uint32_t color;

  for (int i = 0; i < buffer_size; i++) {
    int x = chip->active_column;
    int y = chip->active_page;
    if (chip->scanning_direction & SCAN_MV) {
      x = chip->scanning_direction & SCAN_MX ? (chip->width - 1 - x) : x;
      y = chip->scanning_direction & SCAN_MY ? (chip->height - 1 - y) : y;
    } else {
      x = chip->scanning_direction & SCAN_MY ? (chip->width - 1 - x) : x;
      y = chip->scanning_direction & SCAN_MX ? (chip->height - 1 - y) : y;
    }

    color = rgb565_to_rgba(buffer16[i]);
    int pix_index = y * chip->width + x;

    if ((chip->radius - y) * (chip->radius - y) +
            (chip->radius - x) * (chip->radius - x) <=
        chip->radius * chip->radius) {
      buffer_write(chip->framebuffer, pix_index * sizeof(color), &color,
                   sizeof(color));
    }

    if (chip->scanning_direction & SCAN_MV) {
      chip->active_page++;
      if (chip->active_page > chip->page_end) {
        chip->active_page = chip->page_start;
        chip->active_column++;
        if (chip->active_column > chip->column_end) {
          chip->active_column = chip->column_start;
        }
      }
    } else {
      chip->active_column++;
      if (chip->active_column > chip->column_end) {
        chip->active_column = chip->column_start;
        chip->active_page++;
        if (chip->active_page > chip->page_end) {
          chip->active_page = chip->page_start;
        }
      }
    }
  }
}

void chip_spi_done(void *user_data, uint8_t *buffer, uint32_t count) {
  chip_state_t *chip = (chip_state_t *)user_data;
  if (!count) {
    // This means that we got here from spi_stop, and no data was received
    return;
  }

  if (chip->mode == MODE_DATA) {
    if (chip->ram_write) {
      process_data(chip, (const uint16_t *)buffer, count / 2);
    } else {
      process_command_args(chip, buffer, count);
    }
  } else {
    process_command(chip, buffer, count);
  }

  if (pin_read(chip->cs_pin) == LOW) {
    // Receive the next buffer
    spi_start(chip->spi, chip->spi_buffer, sizeof(chip->spi_buffer));
  }
}