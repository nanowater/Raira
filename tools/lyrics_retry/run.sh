#!/usr/bin/env bash
# The bridge's lyrics lookup against a fake LRCLIB that breaks in different ways (see fake_lrclib.py): one lost request must not turn
# into "no lyrics" (it used to, and the bridge remembered that for a week), a blip is retried once, a lasting failure is an error that
# is not remembered. Uses port 18398 and a throw-away cache directory; needs bridge/target/debug/music-bridge (cargo build).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$ROOT/bridge/target/debug/music-bridge"
[ -x "$BIN" ] || { echo "build the bridge first: cd bridge && cargo build"; exit 1; }
fail=0
case_() { # label, fake mode, bad calls, expected output, expected cache files
  local W; W=$(mktemp -d)
  python3 -I "$ROOT/tools/lyrics_retry/fake_lrclib.py" "$2" "$3" 18398 & local P=$!
  sleep 0.7
  local out; out=$(MUSIC_BRIDGE_LRCLIB=http://127.0.0.1:18398/api/ XDG_CACHE_HOME=$W "$BIN" --lyrics-test-full Song Artist Album 200 2>/dev/null)
  local files; files=$(ls "$W/music-bridge/lyrics" 2>/dev/null | wc -l)
  if [ "${out%% *}" = "$4" ] && [ "$files" = "$5" ]; then echo "ok   $1 -> $out, $files cache file(s)"; else echo "FAIL $1 -> '$out', $files cache file(s) (expected $4, $5)"; fail=1; fi
  kill -9 $P 2>/dev/null; wait $P 2>/dev/null; rm -rf "$W"
}
case_ "search fine"                          drop 0  found 1
case_ "search connection lost once"          drop 1  found 1
case_ "search answered 500 once"             err  1  found 1
case_ "search lost twice in a row"           drop 2  error 0
case_ "search broken for good"               drop 99 error 0
exit $fail
