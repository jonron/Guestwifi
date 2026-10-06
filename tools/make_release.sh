#!/usr/bin/env bash
# Bygger färdiga filer för flashning från webbläsaren (ingen verktygsinstallation hos mottagaren).
#   release/guestwifi-esp32c3-full.bin  – adress 0x0: bootloader + app + typsnitt (första installationen,
#                                         raderar sparade inställningar)
#   release/guestwifi-esp32c3-app.bin   – adress 0x10000: bara appen (uppdatering, behåller inställningar)
set -euo pipefail
cd "$(dirname "$0")/.."

pio run -e esp32c3
pio run -e esp32c3 -t buildfs

B=.pio/build/esp32c3
FW="$HOME/.platformio/packages/framework-arduinoespressif32"
ESPTOOL=$(command -v esptool || command -v esptool.py || echo "$HOME/.platformio/penv/bin/esptool")
mkdir -p release

# Adresser enligt huge_app.csv
"$ESPTOOL" --chip esp32c3 merge-bin -o release/guestwifi-esp32c3-full.bin \
  --flash-mode dio --flash-size 4MB \
  0x0      "$B/bootloader.bin" \
  0x8000   "$B/partitions.bin" \
  0xe000   "$FW/tools/partitions/boot_app0.bin" \
  0x10000  "$B/firmware.bin" \
  0x310000 "$B/littlefs.bin"
cp "$B/firmware.bin" release/guestwifi-esp32c3-app.bin

( cd release && sha256sum guestwifi-esp32c3-*.bin > SHA256SUMS )
ls -la release
