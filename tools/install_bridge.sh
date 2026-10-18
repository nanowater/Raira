#!/usr/bin/env bash
# Install the bridge for the current user (Linux) and start it at every login as a systemd user service.
#
#   tools/install_bridge.sh                 build (release), install to ~/.local/bin, register and start the service
#   tools/install_bridge.sh --no-autostart  only build and install the program
#   tools/install_bridge.sh --status        what is installed and whether the service runs
#   tools/install_bridge.sh --uninstall     stop and remove the service and the program (settings and cache are kept)
#   tools/install_bridge.sh --uninstall --purge   the same and also delete the settings (pairing secrets, token) and the cache
#   (add --yes to --purge to skip the question)
#   tools/install_bridge.sh --dry-run       show what would be done, change nothing
#
# Running it again updates an installed bridge. The settings (~/.config/music-bridge/) are never touched.
# For tests: --no-systemctl writes the files but does not talk to systemd; XDG_BIN_HOME and XDG_CONFIG_HOME move the destinations.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN_DIR=${XDG_BIN_HOME:-$HOME/.local/bin}
BIN="$BIN_DIR/music-bridge"
UNIT_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
UNIT="$UNIT_DIR/music-bridge.service"
ACTION=install; AUTOSTART=1; DRY=0; SYSTEMCTL=1; PURGE=0; YES=0
CONF_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/music-bridge
CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/music-bridge
for a in "$@"; do
  case $a in
    --uninstall) ACTION=uninstall;;
    --status) ACTION=status;;
    --purge) PURGE=1;;
    --yes|-y) YES=1;;
    --no-autostart) AUTOSTART=0;;
    --dry-run) DRY=1;;
    --no-systemctl) SYSTEMCTL=0;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "unknown option: $a (see --help)" >&2; exit 2;;
  esac
done

say()  { printf '%s\n' "$*"; }
step() { if [ "$DRY" = 1 ]; then say "would: $*"; else say "-> $*"; fi; }
run()  { if [ "$DRY" = 1 ]; then say "   $*"; else "$@"; fi; }
ctl()  { [ "$SYSTEMCTL" = 1 ] && run systemctl --user "$@" || true; }

unit_text() {
  cat <<EOF
[Unit]
Description=Music Bridge (now-playing source for the music controller)
# the tray icon needs the desktop session; the program itself only needs the user's D-Bus
After=graphical-session.target
PartOf=graphical-session.target

[Service]
ExecStart=$BIN
# a crash comes back after 3 s; "already running" and a normal stop end with exit code 0 and stay stopped
Restart=on-failure
RestartSec=3

[Install]
WantedBy=graphical-session.target
EOF
}

case $ACTION in
status)
  [ -x "$BIN" ] && say "program:  $BIN" || say "program:  not installed ($BIN)"
  [ -f "$UNIT" ] && say "service:  $UNIT" || say "service:  not registered"
  if command -v systemctl >/dev/null; then
    say "enabled:  $(systemctl --user is-enabled music-bridge 2>/dev/null || true)"
    say "running:  $(systemctl --user is-active music-bridge 2>/dev/null || true)"
  fi
  say "settings: http://127.0.0.1:18801/   log: journalctl --user -u music-bridge -f"
  exit 0;;

uninstall)
  step "stop and disable the service"
  ctl disable --now music-bridge
  step "remove $UNIT and $BIN"
  run rm -f "$UNIT" "$BIN"
  ctl daemon-reload
  if [ "$PURGE" = 1 ]; then
    say "--purge also deletes: $CONF_DIR (the pairing secrets of your devices, the access token, the player settings) and $CACHE_DIR (lyrics cache)."
    if [ "$YES" = 0 ] && [ "$DRY" = 0 ]; then
      printf 'Delete them? [y/N] '; read -r answer || answer=n
      case $answer in y|Y|yes) ;; *) say "kept. The program and the service are removed; settings and cache stay."; exit 0;; esac
    fi
    # a bridge that is still running (started by hand) writes its settings again from memory, so they would come back
    if [ "$DRY" = 0 ] && pgrep -x music-bridge >/dev/null 2>&1; then
      say "warning: a music-bridge is still running (one you started by hand?). Stop it first, or it writes its settings again." >&2
    fi
    step "delete $CONF_DIR and $CACHE_DIR"
    run rm -rf "$CONF_DIR" "$CACHE_DIR"
    if [ "$DRY" = 1 ]; then say "dry run: nothing was changed."; exit 0; fi
    say "done. Nothing of the bridge is left on this PC."
    say "The device still lists this PC under Settings > Sources: remove it there (tap Remove)."
  else
    say "done. Settings and cache stay: $CONF_DIR and $CACHE_DIR (--purge deletes them too)."
  fi
  exit 0;;
esac

# ---- install ----
command -v cargo >/dev/null || { [ "$DRY" = 1 ] || { echo "cargo is needed to build the bridge (https://rustup.rs)" >&2; exit 1; }; }
if [ "$AUTOSTART" = 1 ] && [ "$SYSTEMCTL" = 1 ]; then
  command -v systemctl >/dev/null || { echo "systemctl not found: use --no-autostart and start the bridge yourself" >&2; exit 1; }
fi

step "build the bridge (cargo build --release)"
run cargo build --release --manifest-path "$ROOT/bridge/Cargo.toml"

step "install the program to $BIN"
run mkdir -p "$BIN_DIR"
# copy next to the target and move it over: replacing a program that is running works, copying onto it does not
run install -m755 "$ROOT/bridge/target/release/music-bridge" "$BIN.new"
run mv -f "$BIN.new" "$BIN"
case ":$PATH:" in *":$BIN_DIR:"*) ;; *) say "note: $BIN_DIR is not in your PATH (the service does not need it)";; esac

if [ "$AUTOSTART" = 0 ]; then
  say "installed. Start it with: $BIN   (settings: http://127.0.0.1:18801/)"
  exit 0
fi

step "write the service $UNIT"
if [ "$DRY" = 1 ]; then unit_text | sed 's/^/   | /'; else mkdir -p "$UNIT_DIR"; unit_text > "$UNIT"; fi
ctl daemon-reload
step "enable it for every login and (re)start it now"
ctl enable music-bridge
ctl restart music-bridge

if [ "$DRY" = 0 ] && [ "$SYSTEMCTL" = 1 ]; then
  sleep 2
  if [ "$(systemctl --user is-active music-bridge || true)" = active ]; then
    say "running. Settings: http://127.0.0.1:18801/   log: journalctl --user -u music-bridge -f"
  else
    say "the service is not running. A bridge you started by hand (cargo run) holds the port: stop it and run: systemctl --user restart music-bridge" >&2
    journalctl --user -u music-bridge -n 5 --no-pager 2>/dev/null >&2 || true
    exit 1
  fi
fi
