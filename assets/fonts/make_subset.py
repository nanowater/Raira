#!/usr/bin/env python3
"""Regenerate NotoSansKR-{Regular,Bold}-subset.otf from the system's Noto Sans CJK (face 1 = KR).

    pip install fonttools           # or: pip install --target DIR fonttools; PYTHONPATH=DIR python3 ...
    python3 assets/fonts/make_subset.py [DIR_WITH_NotoSansCJK-*.ttc]     # default /usr/share/fonts/noto-cjk

The Han set is every ideograph that decodes in Python's euc_kr (KS X 1001 hanja, lead bytes 0xCA-0xFD) or shift_jis (JIS X 0208
levels 1 and 2, lead bytes 0x88-0x9F and 0xE0-0xEA, trail bytes 0x40-0xFC). An earlier build missed 132 of the JIS ones (洒, 滲, ...), which
showed up as blank glyphs in lyrics; check_coverage() below fails the run if anything of the set is still missing. Costs ~0.04 MB per face
more than that build. Chinese was added later (2026-11-03): GB2312 (+3291 characters the other two sets lack) and Big5 level 1 (+381 more), about
+0.94 MB per face; Big5 level 2 and GBK would not fit the flash (6.1 and 7.3 MB per face). Characters outside JIS X 0208 (level 3/4, +2918 kanji, +0.85 MB per face) are not included: none of 104 real lyrics needed them.
"""
import subprocess
import sys
from pathlib import Path

SRC = Path(sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/noto-cjk")
OUT = Path(__file__).parent
BASE = ("U+0020-007E,U+00A0-00FF,U+0100-024F,U+0370-03FF,U+0400-04FF,U+2000-206F,U+2190-21FF,U+2500-257F,U+25A0-25FF,U+2600-26FF,"
        "U+3000-303F,U+3040-30FF,U+3130-318F,U+AC00-D7A3,U+FF00-FFEF")


def is_han(c):
    return "一" <= c <= "鿿" or "㐀" <= c <= "䶿" or "豈" <= c <= "﫿"


def han(codec, leads, trails):
    out = set()
    for hi in leads:
        for lo in trails:
            try:
                c = bytes([hi, lo]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(c) == 1 and is_han(c):
                out.add(ord(c))
    return out


def wanted():
    return (
        han("euc_kr", range(0xCA, 0xFE), range(0x40, 0xFF))
        | han("shift_jis", [*range(0x88, 0xA0), *range(0xE0, 0xEB)], range(0x40, 0xFD))
        # Chinese: GB2312 (simplified, 6763 characters) and Big5 level 1 (the 5401 most common traditional ones)
        | han("gb2312", range(0xB0, 0xF8), range(0xA1, 0xFF))
        | han("big5", range(0xA4, 0xC7), range(0x40, 0xFF))
    )


def check_coverage(path, han_set):
    from fontTools.ttLib import TTFont
    have = set(TTFont(path).getBestCmap())
    missing = sorted(c for c in han_set if c not in have)
    if missing:
        sys.exit(f"{path.name}: {len(missing)} wanted kanji are missing: {''.join(map(chr, missing[:40]))}")


def main():
    han_set = wanted()
    unicodes = BASE + "," + ",".join(f"U+{c:04X}" for c in sorted(han_set))
    for weight in ("Regular", "Bold"):
        out = OUT / f"NotoSansKR-{weight}-subset.otf"
        subprocess.run([sys.executable, "-m", "fontTools.subset", str(SRC / f"NotoSansCJK-{weight}.ttc"), "--font-number=1",
                        f"--unicodes={unicodes}", "--layout-features=", f"--output-file={out}"], check=True)
        check_coverage(out, han_set)
        print(f"{out.name}: {out.stat().st_size / 1048576:.2f} MB, {len(han_set)} Han")


main()
