#!/usr/bin/env bash
# Simulerar hela kedjan på datorn: mock-UDM + firmwarens kärnlogik + skyltsidan.
#   sim/run_sim.sh            kör scenarierna
#   sim/run_sim.sh --serve    ...och serverar skyltsidan på http://localhost:8080
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=18443
OUT=sim/out
JSON_INC=.pio/libdeps/native/ArduinoJson/src
if [ ! -d "$JSON_INC" ]; then
  pio pkg install -e native >/dev/null
fi

mkdir -p "$OUT/api" "$OUT/fonts"
cp data/fonts/*.woff2 "$OUT/fonts/"
[ -f data/bg.jpg ] && cp data/bg.jpg "$OUT/bg.jpg"

g++ -std=gnu++17 -O1 -Wall -Wno-maybe-uninitialized -Ilib/GuestCore/src -isystem "$JSON_INC" \
  lib/GuestCore/src/*.cpp sim/host_sim.cpp -o "$OUT/host_sim"

python3 sim/mock_udm.py --port "$PORT" > "$OUT/udm.log" 2>&1 &
UDM_PID=$!
trap 'kill $UDM_PID 2>/dev/null || true' EXIT
for _ in $(seq 50); do curl -s "http://127.0.0.1:$PORT/__sim/state" >/dev/null && break; sleep 0.1; done

"$OUT/host_sim" "$PORT" "$OUT"

echo
echo "--- UDM-loggen ---"
cat "$OUT/udm.log"

if [ "${1:-}" = "--serve" ]; then
  echo "Skyltsidan: http://localhost:8080"
  python3 -m http.server 8080 -d "$OUT"
fi
