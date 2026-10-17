#!/usr/bin/env bash
# Builds the mDNS responder harness, announces a test service and asks it questions. Needs g++, python3 and a network interface
# with multicast (a loopback-only machine has none: the responder then refuses to start).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PORT=${PORT:-18397}
TMP=$(mktemp -d)
PID=""
cleanup() { [ -n "$PID" ] && { kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; }; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

g++ -std=gnu++17 -Wall -Wextra -I"$ROOT/src" "$ROOT/tools/mdns_probe/main.cpp" "$ROOT/src/platform_sim/mdns_responder.cpp" \
    -o "$TMP/mdns_test" -lpthread || exit 2
"$TMP/mdns_test" "$PORT" 15 > "$TMP/out.log" 2>&1 &
PID=$!
for _ in $(seq 20); do grep -qE "announcing|cannot start" "$TMP/out.log" && break; sleep 0.2; done
if grep -q "cannot start" "$TMP/out.log"; then echo "the responder could not start: no multicast-capable network interface?"; exit 2; fi
sleep 0.5
python3 -I "$ROOT/tools/mdns_probe/query.py" "$PORT"
