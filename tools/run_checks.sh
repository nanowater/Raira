#!/usr/bin/env bash
# The automatic checks, one after the other. None of them touches your settings or needs a real player or a real device.
#
#   tools/run_checks.sh              unit tests of the bridge, formatting and lint, WebSocket server, mDNS responder, the lyrics lookup against a failing fake
#                                    LRCLIB, the device's Automatic focus and
#                                    its cache after a reconnect and its lyrics retry (the device checks need a built build/bin/lvglsim, Xvfb and ImageMagick;
#                                    they are skipped without them)
#
# What is not automatic: how the screen looks. tools/ui_run.sh takes screenshots of fake-source scenarios for that.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
results=()
run() {
  local name=$1; shift
  echo; echo "=== $name"
  if "$@"; then results+=("ok     $name"); else results+=("FAILED $name"); fi
}

run "bridge unit tests (cargo test)" bash -c "cd '$ROOT/bridge' && cargo test 2>&1 | tail -n 5 && cargo test >/dev/null 2>&1"
run "formatting and lint (format_check: clang-format, cargo fmt, clippy)" "$ROOT/tools/format_check.sh"
run "WebSocket server (ws_conformance)" "$ROOT/tools/ws_conformance/run.sh"
run "mDNS responder (mdns_probe)" "$ROOT/tools/mdns_probe/run.sh"
run "firmware font image format (font_image)" "$ROOT/tools/font_image/run.sh"
if [ -x "$ROOT/build/bin/lvglsim" ] && command -v Xvfb >/dev/null && command -v import >/dev/null; then
  run "device: Automatic follows the source's pick (ui_run pick)" "$ROOT/tools/ui_run.sh" --scenario pick --shots 1
  run "device: cover and lyrics are kept across a reconnect (ui_run reconnect)" "$ROOT/tools/ui_run.sh" --scenario reconnect --features cover,lyrics --shots 1
  run "device: a failed lyrics lookup is asked again (ui_run lyrics_retry)" "$ROOT/tools/ui_run.sh" --scenario lyrics_retry --features lyrics --shots 1
  run "device: no lyrics is not asked again (ui_run lyrics_none)" "$ROOT/tools/ui_run.sh" --scenario lyrics_none --features lyrics --shots 1
  run "device: a new track ends the retrying (ui_run lyrics_retry_track_change)" "$ROOT/tools/ui_run.sh" --scenario lyrics_retry_track_change --features lyrics --shots 1
else
  results+=("skipped device checks (need build/bin/lvglsim, Xvfb, ImageMagick)")
fi
run "lyrics: a lost request is not \"no lyrics\" (lyrics_retry)" "$ROOT/tools/lyrics_retry/run.sh"
if [ -f "${IDF_PATH:-$HOME/projects/esp-idf}/export.sh" ]; then
  run "firmware: the core compiles for the ESP32-S3 (firmware_build.sh)" "$ROOT/tools/firmware_build.sh"
else
  results+=("skipped firmware build (ESP-IDF not found, see docs/PORTING.md)")
fi

echo; echo "=== summary"
printf '%s\n' "${results[@]}"
for r in "${results[@]}"; do case "$r" in FAILED*) exit 1;; esac; done
