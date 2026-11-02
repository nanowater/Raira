#!/usr/bin/env bash
# Builds the firmware (firmware/, ESP-IDF) to check that the portable core still compiles and links for the ESP32-P4.
# Needs ESP-IDF (default ~/projects/esp-idf; IDF_PATH wins); exit code 2 when it is not installed.
#
#   tools/firmware_build.sh                  build the P4 (the first time: set-target first), show the size summary
#   tools/firmware_build.sh --clean          start from nothing (deletes the build directory and sdkconfig, so that a changed
#                                            sdkconfig.defaults* is read: they are only read when sdkconfig is created)
#
# P4 = firmware/build_p4 + sdkconfig.p4
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IDF=${IDF_PATH:-$HOME/projects/esp-idf}
[ -f "$IDF/export.sh" ] || { echo "ESP-IDF not found at $IDF (see docs/PORTING.md)"; exit 2; }
clean=0
[ "${1:-}" = "--clean" ] && { clean=1; shift; }
chips=${1:-esp32p4}
cd "$ROOT/firmware" || exit 1
. "$IDF/export.sh" > /dev/null 2>&1 || { echo "could not activate ESP-IDF"; exit 2; }

build_chip() {
  local chip=$1 args=()
  case "$chip" in
    esp32p4) bdir=build_p4; cfg=sdkconfig.p4 ;;
    *) echo "unknown chip: $chip (esp32p4)"; return 1 ;;
  esac
  args=(-B "$bdir" -DSDKCONFIG="$cfg")
  [ $clean = 1 ] && rm -rf "$bdir" "$cfg" "$cfg.old"
  [ -f "$cfg" ] || idf.py "${args[@]}" set-target "$chip" > "/tmp/firmware_set_target_$chip.log" 2>&1 \
    || { tail -20 "/tmp/firmware_set_target_$chip.log"; return 1; }
  local log; log=$(mktemp)
  if idf.py "${args[@]}" build > "$log" 2>&1; then
    echo "$chip: $(grep 'raira.bin binary size' "$log" | tail -1)"
    echo "$chip warnings: $(grep -c 'warning:' "$log")"
    rm -f "$log"
  else
    grep -n -B2 -A6 "error:" "$log" | head -60
    echo "($chip: full log: $log)"
    return 1
  fi
}

rc=0
for c in $chips; do build_chip "$c" || rc=1; done
exit $rc
