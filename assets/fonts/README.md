# Fonts

`NotoSansKR-Regular-subset.otf` — Noto Sans CJK KR Regular (SIL OFL 1.1), subset to Latin,
punctuation, kana, Hangul jamo/syllables, fullwidth forms (！？～…), Latin Extended, Greek, Cyrillic, arrows, geometric shapes and misc symbols (★♪♡), and ~7k Han characters (KS X 1001 hanja + JIS X 0208 kanji). Embedded via `src/fonts/noto_sans_kr.c` and rendered
with LVGL TinyTTF at runtime (any size, one file).

Regenerate with `python3 assets/fonts/make_subset.py` (needs `pip install fonttools` and the system's Noto Sans CJK, default
`/usr/share/fonts/noto-cjk`). The Han set is every CJK ideograph that decodes in Python's `euc_kr` (lead bytes 0xCA-0xFD) or `shift_jis`
(lead bytes 0x88-0x9F, 0xE0-0xEA, trail bytes 0x40-0xFC) codecs; the script fails if one of them is missing from the result. (An earlier hand-made build
lacked 132 JIS X 0208 kanji such as 洒 and 滲, which showed up as blank glyphs in lyrics; box-drawing characters U+2500-257F were added at the same time.)
Kanji outside JIS X 0208 (JIS X 0213 level 3/4, +2918 kanji) would cost about 0.85 MB per face and none of 104 real lyrics needed them.

`NotoSansKR-Bold-subset.otf` is the same glyph set from `NotoSansCJK-Bold.ttc` (used for lyrics);
the script builds both.

Simplified-Chinese-only characters (GB2312, ~+0.9MB) are not included. The font is the KR face, so
Japanese kanji are drawn with Korean glyph variants.
`src/fonts/font_mono_16.c` (DejaVu Sans Mono, digits and ":") is generated with lv_font_conv; see its header.
