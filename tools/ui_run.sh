#!/usr/bin/env bash
# Runs the simulator as the hub on a virtual display (Xvfb), plays a fake-source scenario into it and takes screenshots, so a
# device-side change can be looked at without a real player or a real window. Everything it starts is stopped again by PID.
#
#   tools/ui_run.sh --scenario pick --shots 3,8,13
#   tools/ui_run.sh --scenario boundary --shots 6,11,15 --out /tmp/shots
#
# Options: --scenario NAME (see tools/fake_source.py: playing, idle, boundary, pick)   --scenario-file FILE
#          --shots S1,S2,..  seconds after the source is connected (default 3)         --out DIR  (default $TMPDIR/music-controller-shots)
#          --port N (default 18399)   --features cover,lyrics   --keep-log (print the simulator log at the end)
# Needs: a built build/bin/lvglsim, Xvfb, ImageMagick's `import`, python3. The simulator runs with a throw-away HOME in which the
# fake source is already paired (tools/seed_device.py), so nothing of your own settings is touched.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SIM="$ROOT/build/bin/lvglsim"
SCENARIO=playing; SCENARIO_FILE=""; SHOTS=3; PORT=18399; FEATURES=""; KEEP_LOG=0
OUT="${TMPDIR:-/tmp}/music-controller-shots"
while [ $# -gt 0 ]; do
  case "$1" in
    --scenario) SCENARIO=$2; shift 2;;
    --scenario-file) SCENARIO_FILE=$2; shift 2;;
    --shots) SHOTS=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --port) PORT=$2; shift 2;;
    --features) FEATURES=$2; shift 2;;
    --keep-log) KEEP_LOG=1; shift;;
    *) echo "unknown option $1"; exit 2;;
  esac
done
for need in Xvfb import python3; do command -v "$need" >/dev/null || { echo "missing: $need"; exit 2; }; done
[ -x "$SIM" ] || { echo "build the simulator first: cd build && ninja"; exit 2; }
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then echo "port $PORT is in use (--port N)"; exit 2; fi

TMP=$(mktemp -d)
PIDS=()
# kill -9 only sends the signal: wait until the processes are really gone, or the next run finds the port still taken
cleanup() { for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
mkdir -p "$OUT"

# a free X display number
DISP=""
for n in $(seq 70 99); do
  if [ ! -e "/tmp/.X$n-lock" ] && [ ! -e "/tmp/.X11-unix/X$n" ]; then DISP=:$n; break; fi
done
[ -n "$DISP" ] || { echo "no free X display"; exit 2; }
Xvfb "$DISP" -screen 0 800x480x24 > "$TMP/xvfb.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 30); do [ -e "/tmp/.X11-unix/X${DISP#:}" ] && break; sleep 0.1; done

python3 -I "$ROOT/tools/seed_device.py" "$TMP/home" > /dev/null
DISPLAY=$DISP SDL_VIDEODRIVER=x11 HOME="$TMP/home" "$SIM" -S "$PORT" > "$TMP/sim.log" 2>&1 &
SIMPID=$!
PIDS+=($SIMPID)
for _ in $(seq 40); do ss -ltn 2>/dev/null | grep -q ":$PORT " && break; kill -0 $SIMPID 2>/dev/null || break; sleep 0.1; done
if ! kill -0 $SIMPID 2>/dev/null; then echo "the simulator did not start:"; cat "$TMP/sim.log"; exit 1; fi

SRC=(python3 -I "$ROOT/tools/fake_source.py" --port "$PORT" --features "$FEATURES")
if [ -n "$SCENARIO_FILE" ]; then SRC+=(--scenario-file "$SCENARIO_FILE"); else SRC+=(--scenario "$SCENARIO"); fi
"${SRC[@]}" > "$TMP/source.log" 2>&1 &
SRCPID=$!
PIDS+=($SRCPID)

prev=0
for t in $(echo "$SHOTS" | tr ',' ' '); do
  sleep "$(python3 -I -c "print(max(0, $t - $prev))")"
  DISPLAY=$DISP import -window root "$OUT/${SCENARIO}_${t}s.png" && echo "screenshot: $OUT/${SCENARIO}_${t}s.png"
  prev=$t
done
wait $SRCPID 2>/dev/null; rc=$?
echo "--- fake source:"; cat "$TMP/source.log"
[ "$KEEP_LOG" = 1 ] && { echo "--- simulator:"; cat "$TMP/sim.log"; }
kill -0 $SIMPID 2>/dev/null || { echo "the simulator died during the run:"; cat "$TMP/sim.log"; exit 1; }
exit $rc
