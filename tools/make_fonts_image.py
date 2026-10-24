#!/usr/bin/env python3
"""Pack the two Noto Sans KR subsets into the image for the firmware's `fonts` flash partition (format: src/fonts/font_image.h).

  python3 tools/make_fonts_image.py                       -> firmware/build/fonts.bin
  python3 tools/make_fonts_image.py --out /tmp/fonts.bin

Write it to the board's `fonts` partition (not part of the app, so an app update never touches it), with the port of the board:
  python ~/projects/esp-idf/components/partition_table/parttool.py --port /dev/ttyACM0 write_partition --partition-name fonts --input firmware/build/fonts.bin
or as part of the merged image of the web flasher, at the partition's offset in firmware/partitions.csv.
The script fails when the image does not fit the partition."""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAGIC = b"MCFT"
VERSION = 1
ALIGN = 16


def align(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def build(regular: bytes, bold: bytes) -> bytes:
    header = 12 + 2 * 8
    off_r = align(header)
    off_b = align(off_r + len(regular))
    out = bytearray(MAGIC + struct.pack("<II", VERSION, 2))
    out += struct.pack("<II", off_r, len(regular)) + struct.pack("<II", off_b, len(bold))
    out += bytes(off_r - len(out)) + regular
    out += bytes(off_b - len(out)) + bold
    return bytes(out)


def partition_size(csv: Path, name: str) -> int:
    for line in csv.read_text().splitlines():
        cols = [c.strip() for c in line.split(",")]
        if line.strip() and not line.startswith("#") and cols[0] == name:
            return int(cols[4], 0)
    sys.exit(f"{csv}: no partition named {name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--regular", default=ROOT / "assets/fonts/NotoSansKR-Regular-subset.otf", type=Path)
    ap.add_argument("--bold", default=ROOT / "assets/fonts/NotoSansKR-Bold-subset.otf", type=Path)
    ap.add_argument("--out", default=ROOT / "firmware/build/fonts.bin", type=Path)
    ap.add_argument("--partitions", default=ROOT / "firmware/partitions.csv", type=Path)
    a = ap.parse_args()

    image = build(a.regular.read_bytes(), a.bold.read_bytes())
    limit = partition_size(a.partitions, "fonts")
    if len(image) > limit:
        sys.exit(f"the image is {len(image)} bytes, the fonts partition only {limit}")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(image)
    print(f"{a.out}: {len(image)} bytes of {limit} ({len(image) * 100 // limit} %), {limit - len(image)} bytes free")


main()
