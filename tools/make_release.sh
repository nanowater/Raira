#!/usr/bin/env bash
# Builds what a release contains, into dist/ (git-ignored). Nothing is uploaded or tagged.
#
#   tools/make_release.sh [VERSION]      VERSION defaults to `git describe`
#
#   dist/raira-p4-VERSION.bin            the whole flash of the board in one image (bootloader, partition table, app, fonts):
#                                        esptool --chip esp32p4 -p PORT write-flash 0x0 raira-p4-VERSION.bin
#   dist/raira-bridge-VERSION-linux-x86_64.tar.gz   the bridge (release build) + install.sh (service, autostart)
#   dist/SHA256SUMS
#
# Needs ESP-IDF (IDF_PATH, default ~/projects/esp-idf) for the firmware and cargo for the bridge. Run from a clean checkout
# of the commit that is to be released: the version is taken from git and the firmware embeds it.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IDF=${IDF_PATH:-$HOME/projects/esp-idf}
VERSION=${1:-$(git -C "$ROOT" describe --tags --always --dirty)}
DIST=$ROOT/dist
BUILD=$ROOT/firmware/build_p4
FLASH_SIZE_BYTES=$((16 * 1024 * 1024))

[ -f "$IDF/export.sh" ] || { echo "ESP-IDF not found at $IDF" >&2; exit 2; }
case $VERSION in *-dirty) echo "warning: the tree has uncommitted changes ($VERSION)" >&2;; esac
rm -rf "$DIST"; mkdir -p "$DIST"

echo "== firmware"
"$ROOT/tools/firmware_build.sh"
python3 "$ROOT/tools/make_fonts_image.py" --out "$BUILD/fonts.bin"
FONTS_OFFSET=$(awk -F',' '$1 == "fonts" { gsub(/ /, "", $4); print $4 }' "$ROOT/firmware/partitions.csv")
(
  . "$IDF/export.sh" > /dev/null 2>&1
  cd "$BUILD"
  # flasher_args.json lists the offsets of bootloader, partition table, otadata and app; the fonts partition is not part of it
  python3 - "$FONTS_OFFSET" <<'EOF' > "$DIST/merge_args"
import json, sys
a = json.load(open("flasher_args.json"))
args = [f"{off} {path}" for off, path in a["flash_files"].items()]
args.append(f"{sys.argv[1]} fonts.bin")
print(" ".join(args))
EOF
  # shellcheck disable=SC2046
  python -m esptool --chip esp32p4 merge-bin -o "$DIST/raira-p4-$VERSION.bin" --flash-mode dio --flash-size 16MB \
    --flash-freq 80m $(cat "$DIST/merge_args")
)
rm -f "$DIST/merge_args"
size=$(stat -c %s "$DIST/raira-p4-$VERSION.bin")
echo "image: $size bytes (flash: $FLASH_SIZE_BYTES)"
[ "$size" -le "$FLASH_SIZE_BYTES" ] || { echo "image does not fit the 16 MB flash" >&2; exit 1; }

echo "== bridge"
cargo build --release --manifest-path "$ROOT/bridge/Cargo.toml"
PKG=raira-bridge-$VERSION-linux-x86_64
mkdir -p "$DIST/$PKG"
cp "$ROOT/bridge/target/release/raira-bridge" "$DIST/$PKG/"
cp "$ROOT/tools/install_bridge.sh" "$DIST/$PKG/install_bridge.sh"
cat > "$DIST/$PKG/install.sh" <<'EOF'
#!/usr/bin/env bash
# Installs the bridge for this user and starts it at every login. Options: see ./install_bridge.sh --help
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/install_bridge.sh" --binary "$here/raira-bridge" "$@"
EOF
chmod +x "$DIST/$PKG/install.sh" "$DIST/$PKG/install_bridge.sh"
tar -C "$DIST" -czf "$DIST/$PKG.tar.gz" "$PKG"
rm -rf "${DIST:?}/$PKG"

(cd "$DIST" && sha256sum -- * > SHA256SUMS)
echo "== done"
ls -l "$DIST"
