#!/usr/bin/env bash
# Builds the WsServer harness and checks it with an independent client. Needs g++, libwebsockets (pkg-config), python3.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PORT=${PORT:-18398}
TMP=$(mktemp -d)
PID=""
cleanup() { [ -n "$PID" ] && { kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; }; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

if ss -ltn 2>/dev/null | grep -q ":$PORT "; then echo "port $PORT is in use (set PORT=...)"; exit 2; fi
g++ -std=gnu++17 -Wall -Wextra -I"$ROOT/src" "$ROOT/tools/ws_conformance/server_main.cpp" "$ROOT/src/platform_sim/ws_server_lws.cpp" \
    -o "$TMP/ws_server" $(pkg-config --cflags --libs libwebsockets) -lpthread || exit 2
"$TMP/ws_server" "$PORT" 15 > "$TMP/server.log" 2>&1 &
PID=$!
for _ in $(seq 20); do grep -q listening "$TMP/server.log" && break; sleep 0.2; done
python3 -I "$ROOT/tools/ws_conformance/client.py" "$PORT"
