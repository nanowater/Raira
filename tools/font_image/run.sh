#!/usr/bin/env bash
# Tests the format of the firmware's `fonts` partition (src/fonts/font_image.*): builds a real image with make_fonts_image.py and
# parses it, then parses damaged ones. Needs python3 and g++; touches nothing outside a temporary directory.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 -I "$ROOT/tools/make_fonts_image.py" --out "$T/fonts.bin" > /dev/null
g++ -std=c++23 -Wall -Wextra -Werror -I"$ROOT/src" "$ROOT/tools/font_image/test.cpp" "$ROOT/src/fonts/font_image.cpp" -o "$T/test"
"$T/test" "$T/fonts.bin" "$ROOT/assets/fonts/NotoSansKR-Regular-subset.otf" "$ROOT/assets/fonts/NotoSansKR-Bold-subset.otf"
