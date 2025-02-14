# GC9A01 Chip

The GC9A01 is a simulation chip for circular displays, designed for use with the [Wokwi](https://wokwi.com/) simulator.

## Pin names

| Name | Description                |
| ---- | -------------------------- |
| VCC  | Supply voltage             |
| GND  | Ground                     |
| DIN  | Data IN                    |
| CLK  | Clock                      |
| CS   | Chip Select                |
| DC   | Data/Command               |
| RST  | Reset                      |

## Usage

To use this chip in your project, include it as a dependency in your `diagram.json` file:

```json
"dependencies": {
  "chip-gc9a01": "github:v0lkc/wokwi-chip-GC9A01@1.0.0"
}
```

Then, add the chip to your circuit by adding a `gc9a01` item to the `parts` section of `diagram.json`:

```json
"parts": {
  ...,
  { "type": "chip-gc9a01", "id": "display" }
},
```

## License

This project is licensed under the Apache-2.0 license. See the [LICENSE](LICENSE) file for more details.
