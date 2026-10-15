# Fonts

`NotoSansKR-Regular-subset.otf` — Noto Sans CJK KR Regular (SIL OFL 1.1), subset to Latin,
punctuation, kana, Hangul jamo/syllables. Embedded via `src/fonts/noto_sans_kr.c` and rendered
with LVGL TinyTTF at runtime (any size, one file).

Regenerate (needs `pip install fonttools`):

    pyftsubset NotoSansCJK-Regular.ttc --font-number=1 \
      --unicodes="U+0020-007E,U+00A0-00FF,U+2000-206F,U+3000-303F,U+3040-30FF,U+3130-318F,U+AC00-D7A3" \
      --layout-features='' --output-file=NotoSansKR-Regular-subset.otf

Han characters (kanji/hanja) are not included.
`src/fonts/font_mono_13.c` (DejaVu Sans Mono, digits and ":") is generated with lv_font_conv; see its header.
