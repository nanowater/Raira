#!/usr/bin/env bash
# Runs the simulator as the hub on a virtual display (Xvfb), plays a fake-source scenario into it and takes screenshots, so a
# device-side change can be looked at without a real player or a real window. Everything it starts is stopped again by PID.
#
#   tools/ui_run.sh --scenario pick --shots 3,8,13
#   tools/ui_run.sh --scenario boundary --shots 6,11,15 --out /tmp/shots
#
# Options: --scenario NAME (see tools/fake_source.py: playing, idle, boundary, pick)   --scenario-file FILE
#          --shots S1,S2,..  seconds after the source is connected (default 3)         --out DIR  (default $TMPDIR/raira-shots)
#          --key T:NAME (xdotool key, e.g. 5:Down)   --click T:X:Y[:HOLD]  (click at pixel X,Y T seconds in, held HOLD s; repeatable; needs xdotool)   --port N (default 18399)   --features cover,lyrics   --keep-log (print the simulator log at the end)
# Needs: a built build/bin/lvglsim, Xvfb, ImageMagick's `import`, python3. The simulator runs with a throw-away HOME in which the
# fake source is already paired (tools/seed_device.py), so nothing of your own settings is touched.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SIM="$ROOT/build/bin/lvglsim"
CLICKS=""; KEYS="";
SCENARIO=playing; SCENARIO_FILE=""; SHOTS=3; PORT=18399; FEATURES=""; KEEP_LOG=0
OUT="${TMPDIR:-/tmp}/raira-shots"
while [ $# -gt 0 ]; do
  case "$1" in
    --scenario) SCENARIO=$2; shift 2;;
    --scenario-file) SCENARIO_FILE=$2; shift 2;;
    --shots) SHOTS=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --port) PORT=$2; shift 2;;
    --features) FEATURES=$2; shift 2;;
    --click) CLICKS="$CLICKS $2"; shift 2;;
    --key) KEYS="$KEYS $2"; shift 2;;
    --keep-log) KEEP_LOG=1; shift;;
    *) echo "unknown option $1"; exit 2;;
  esac
done
for need in Xvfb import python3 $([ -n "$CLICKS$KEYS" ] && echo xdotool); do command -v "$need" >/dev/null || { echo "missing: $need"; exit 2; }; done
[ -x "$SIM" ] || { echo "build the simulator first: cmake -B build && cmake --build build -j"; exit 2; }
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then echo "port $PORT is in use (--port N)"; exit 2; fi

TMP=$(mktemp -d)
PIDS=()
# kill -9 only sends the signal: wait until the processes are really gone, or the next run finds the port still taken
cleanup() { for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done; [ -n "${DISP:-}" ] && rm -f "/tmp/.X${DISP#:}-lock" "/tmp/.X11-unix/X${DISP#:}"; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
mkdir -p "$OUT"

# a free X display number
DISP=""
for n in $(seq 70 99); do
  # a lock left by an Xvfb that was killed with -9 (its pid is gone) is stale: clear it
  if [ -e "/tmp/.X$n-lock" ] && ! kill -0 "$(tr -d ' ' < "/tmp/.X$n-lock" 2>/dev/null)" 2>/dev/null; then
    rm -f "/tmp/.X$n-lock" "/tmp/.X11-unix/X$n"
  fi
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

# events in time order: "T shot", "T click X Y" (a click happens before a screenshot taken at the same second)
EVENTS=$( { for t in $(echo "$SHOTS" | tr ',' ' '); do echo "$t 1 shot"; done
            for k in $KEYS; do IFS=: read -r t name <<< "$k"; echo "$t 0 key $name"; done
            for c in $CLICKS; do IFS=: read -r t x y h <<< "$c"; echo "$t 0 click $x $y ${h:-0}"; done; } | sort -n -k1,1 -k2,2)
prev=0
while read -r t _ kind x y hold; do
  sleep "$(python3 -I -c "print(max(0, $t - $prev))")"
  if [ "$kind" = key ]; then
    DISPLAY=$DISP xdotool key "$x" && echo "key: $x at ${t}s"
  elif [ "$kind" = click ]; then
    if [ "${hold:-0}" != 0 ]; then
      DISPLAY=$DISP xdotool mousemove "$x" "$y" mousedown 1; sleep "$hold"; DISPLAY=$DISP xdotool mouseup 1
    else
      DISPLAY=$DISP xdotool mousemove "$x" "$y" click 1
    fi
    echo "click: $x,$y at ${t}s"
  else
    DISPLAY=$DISP import -window root "$OUT/${SCENARIO}_${t}s.png" && echo "screenshot: $OUT/${SCENARIO}_${t}s.png"
  fi
  prev=$t
done <<< "$EVENTS"
wait $SRCPID 2>/dev/null; rc=$?
echo "--- fake source:"; cat "$TMP/source.log"
[ "$KEEP_LOG" = 1 ] && { echo "--- simulator:"; cat "$TMP/sim.log"; }
kill -0 $SIMPID 2>/dev/null || { echo "the simulator died during the run:"; cat "$TMP/sim.log"; exit 1; }
exit $rc
