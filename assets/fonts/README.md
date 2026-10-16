# Fonts

`NotoSansKR-Regular-subset.otf` — Noto Sans CJK KR Regular (SIL OFL 1.1), subset to Latin,
punctuation, kana, Hangul jamo/syllables, and ~7k Han characters (KS X 1001 hanja + JIS X 0208 kanji). Embedded via `src/fonts/noto_sans_kr.c` and rendered
with LVGL TinyTTF at runtime (any size, one file).

Regenerate (needs `pip install fonttools`). The Han code points are every CJK ideograph that decodes
in Python's `euc_kr` (lead bytes 0xCA-0xFD) or `shift_jis` (lead bytes 0x88-0x9F, 0xE0-0xEA) codecs,
appended to the unicode list below:

    pyftsubset NotoSansCJK-Regular.ttc --font-number=1 \
      --unicodes="U+0020-007E,U+00A0-00FF,U+2000-206F,U+3000-303F,U+3040-30FF,U+3130-318F,U+AC00-D7A3,<han...>" \
      --layout-features='' --output-file=NotoSansKR-Regular-subset.otf

`NotoSansKR-Bold-subset.otf` is the same glyph set from `NotoSansCJK-Bold.ttc` (used for lyrics);
regenerate it with the same command, swapping the input file.

Simplified-Chinese-only characters (GB2312, ~+0.9MB) are not included. The font is the KR face, so
Japanese kanji are drawn with Korean glyph variants.
`src/fonts/font_mono_16.c` (DejaVu Sans Mono, digits and ":") is generated with lv_font_conv; see its header.
