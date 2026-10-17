#!/usr/bin/env bash
# The lyrics lookup exists twice: src/lyrics.cpp (the device, used as a fallback) and bridge/src/lyrics.rs (the PC, used with the hub).
# This runs both on the songs in songs.tsv (title<TAB>artists<TAB>album<TAB>seconds) and compares the line lists. Needs the network
# (LRCLIB), g++, libcurl and a built bridge (cargo build). LRCLIB rate-limits: a pause between songs, a retry when it does not answer.
# Usage: compare.sh [songs.tsv]    Exit code 0 = identical on every song that could be looked up.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SONGS=${1:-$ROOT/tools/lyrics_compare/songs.tsv}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

BRIDGE="$ROOT/bridge/target/debug/music-bridge"
[ -x "$BRIDGE" ] || (cd "$ROOT/bridge" && cargo build) || exit 2
g++ -std=gnu++17 -Wall -Wextra -I"$ROOT/src" "$ROOT/tools/lyrics_compare/cpp_main.cpp" "$ROOT/src/lyrics.cpp" "$ROOT/src/http_util.cpp" \
    "$ROOT/src/platform_sim/http_curl.cpp" -o "$TMP/cpp_lyrics" -lcurl || exit 2

differ=0; same=0; skipped=0
while IFS=$'\t' read -r title artists album secs; do
  [ -z "$title" ] && continue
  rust=""
  for attempt in 1 2 3; do
    "$TMP/cpp_lyrics" "$title" "$artists" "$album" "$secs" > "$TMP/c.json" 2>/dev/null
    sleep 1.5
    rust=$("$BRIDGE" --lyrics-test "$title" "$artists" "$album" "$secs" 2>/dev/null)
    sleep 1.5
    case "$rust" in *'"error":true'*) sleep $((8 * attempt)); continue;; esac
    break
  done
  verdict=$(python3 -I - "$TMP/c.json" "$rust" "$title" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); r = json.loads(sys.argv[2] or '{"error":true}')
def d(x):
    return "%d lines, synced=%s" % (len(x["lines"]), x["synced"]) if "lines" in x else "found=false"
if r.get("error"):
    print("SKIP   %s: LRCLIB did not answer the Rust lookup" % sys.argv[3])
elif c == r:
    print("SAME   %s (%s)" % (sys.argv[3], d(r)))
else:
    print("DIFFER %s: C++ %s / Rust %s" % (sys.argv[3], d(c), d(r)))
PY
)
  echo "$verdict"
  case "$verdict" in SAME*) same=$((same+1));; DIFFER*) differ=$((differ+1));; *) skipped=$((skipped+1));; esac
done < "$SONGS"
echo; echo "same: $same, different: $differ, skipped: $skipped"
[ "$differ" -eq 0 ]
