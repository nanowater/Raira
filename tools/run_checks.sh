#!/usr/bin/env bash
# The automatic checks, one after the other. None of them touches your settings or needs a real player or a real device.
#
#   tools/run_checks.sh              unit tests of the bridge, WebSocket server, mDNS responder, the lyrics lookup against a failing fake
#                                    LRCLIB, the device's Automatic focus and
#                                    its cache after a reconnect (the last two need a built build/bin/lvglsim, Xvfb and ImageMagick;
#                                    they are skipped without them)
#   tools/run_checks.sh --network    also the C++/Rust lyrics comparison (asks LRCLIB, takes a minute)
#
# What is not automatic: how the screen looks. tools/ui_run.sh takes screenshots of fake-source scenarios for that.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NETWORK=0; [ "${1:-}" = "--network" ] && NETWORK=1
results=()
run() {
  local name=$1; shift
  echo; echo "=== $name"
  if "$@"; then results+=("ok     $name"); else results+=("FAILED $name"); fi
}

run "bridge unit tests (cargo test)" bash -c "cd '$ROOT/bridge' && cargo test 2>&1 | tail -n 5 && cargo test >/dev/null 2>&1"
run "WebSocket server (ws_conformance)" "$ROOT/tools/ws_conformance/run.sh"
run "mDNS responder (mdns_probe)" "$ROOT/tools/mdns_probe/run.sh"
if [ -x "$ROOT/build/bin/lvglsim" ] && command -v Xvfb >/dev/null && command -v import >/dev/null; then
  run "device: Automatic follows the source's pick (ui_run pick)" "$ROOT/tools/ui_run.sh" --scenario pick --shots 1
  run "device: cover and lyrics are kept across a reconnect (ui_run reconnect)" "$ROOT/tools/ui_run.sh" --scenario reconnect --features cover,lyrics --shots 1
else
  results+=("skipped device checks (need build/bin/lvglsim, Xvfb, ImageMagick)")
fi
run "lyrics: a lost request is not \"no lyrics\" (lyrics_retry)" "$ROOT/tools/lyrics_retry/run.sh"
[ "$NETWORK" = 1 ] && run "lyrics: device vs bridge (lyrics_compare)" "$ROOT/tools/lyrics_compare/compare.sh"

echo; echo "=== summary"
printf '%s\n' "${results[@]}"
for r in "${results[@]}"; do case "$r" in FAILED*) exit 1;; esac; done
