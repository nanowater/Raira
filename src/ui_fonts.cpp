#include "ui_fonts.h"
#include <map>

extern "C" const unsigned char * noto_sans_kr_get(size_t * size);      /* embedded Noto Sans KR subset */
extern "C" const unsigned char * noto_sans_kr_bold_get(size_t * size); /* same glyph set, Bold */

const lv_font_t * ui_noto_font(int32_t px, const lv_font_t * fallback, bool bold)
{
    static std::map<std::pair<int32_t, bool>, const lv_font_t *> cache;
    auto key = std::make_pair(px, bold);
    auto it  = cache.find(key);
    if(it != cache.end()) return it->second;

    size_t size                = 0;
    const unsigned char * data = bold ? noto_sans_kr_bold_get(&size) : noto_sans_kr_get(&size);
    lv_font_t * f              = (data && size) ? lv_tiny_ttf_create_data(data, size, px) : nullptr; /* no font data: the fallback */
    const lv_font_t * result   = fallback;
    if(f) {
        f->fallback = fallback; /* glyphs outside the subset (e.g. symbols) */
        result      = f;
    }
    cache[key] = result;
    return result;
}
