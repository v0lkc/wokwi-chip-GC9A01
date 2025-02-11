FROM wokwi/builder-clang-wasm

USER root
RUN apk add gcompat libc6-compat zlib-dev

# Install arduino-cli:
RUN  wget -O /tmp/cli.tar.gz \
        https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz && \
    tar -C /usr/local/bin -zxvf /tmp/cli.tar.gz && \
    rm /tmp/cli.tar.gz
RUN arduino-cli core update-index \
 && arduino-cli core install arduino:avr \
 && arduino-cli lib install "Adafruit GFX Library" \
 && arduino-cli lib install "Adafruit GC9A01A"
