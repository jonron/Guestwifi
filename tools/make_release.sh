#!/usr/bin/env bash
# Bygger färdiga filer för flashning från webbläsaren (ingen verktygsinstallation hos mottagaren).
#   tools/make_release.sh [env] [utkatalog]      env = esp32c5 (standard) eller esp32c3
#
#   <ut>/guestwifi-<env>-full.bin  – adress 0x0: bootloader + app + typsnitt
#                                    (första installationen, raderar sparade inställningar)
#   <ut>/guestwifi-<env>-app.bin   – adress 0x10000: bara appen (uppdatering, behåller inställningar)
set -euo pipefail
cd "$(dirname "$0")/.."

ENV=${1:-esp32c5}
OUT=${2:-release}
case "$ENV" in
  esp32c5) CHIP=esp32c5; BOOT=0x2000 ;;
  esp32c3) CHIP=esp32c3; BOOT=0x0 ;;
  *) echo "okänd env: $ENV" >&2; exit 1 ;;
esac

pio run -e "$ENV"
pio run -e "$ENV" -t buildfs

B=.pio/build/$ENV
FW="$HOME/.platformio/packages/framework-arduinoespressif32"
ESPTOOL=$(command -v esptool || command -v esptool.py || echo "$HOME/.platformio/penv/bin/esptool")
mkdir -p "$OUT"

# Adresser enligt huge_app.csv
"$ESPTOOL" --chip "$CHIP" merge-bin -o "$OUT/guestwifi-$ENV-full.bin" --flash-size keep \
  "$BOOT"  "$B/bootloader.bin" \
  0x8000   "$B/partitions.bin" \
  0xe000   "$FW/tools/partitions/boot_app0.bin" \
  0x10000  "$B/firmware.bin" \
  0x310000 "$B/littlefs.bin"
cp "$B/firmware.bin" "$OUT/guestwifi-$ENV-app.bin"

( cd "$OUT" && sha256sum guestwifi-*.bin > SHA256SUMS )
ls -la "$OUT"
