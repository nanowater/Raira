#!/usr/bin/env bash
# Builds the firmware (firmware/, ESP-IDF) to check that the portable core still compiles and links for the ESP32-S3.
# Needs ESP-IDF (default ~/projects/esp-idf; IDF_PATH wins); exit code 2 when it is not installed.
#
#   tools/firmware_build.sh            build (the first time: set-target first), show the size summary
#   tools/firmware_build.sh --clean    start from nothing (deletes firmware/build and firmware/sdkconfig, so that a changed
#                                      sdkconfig.defaults is read: it is only read when sdkconfig is created)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IDF=${IDF_PATH:-$HOME/projects/esp-idf}
[ -f "$IDF/export.sh" ] || { echo "ESP-IDF not found at $IDF (see docs/PORTING.md)"; exit 2; }
cd "$ROOT/firmware" || exit 1
[ "${1:-}" = "--clean" ] && rm -rf build sdkconfig sdkconfig.old
. "$IDF/export.sh" > /dev/null 2>&1 || { echo "could not activate ESP-IDF"; exit 2; }
[ -f sdkconfig ] || idf.py set-target esp32s3 > /tmp/firmware_set_target.log 2>&1 || { tail -20 /tmp/firmware_set_target.log; exit 1; }
log=$(mktemp)
if idf.py build > "$log" 2>&1; then
  grep "raira.bin binary size" "$log" | tail -1
  grep -c "warning:" "$log" | sed 's/^/warnings: /'
  rm -f "$log"
else
  grep -n -B2 -A6 "error:" "$log" | head -60
  echo "(full log: $log)"
  exit 1
fi
