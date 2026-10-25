#pragma once

#include <cstddef>
#include <cstdint>

/* The two Noto Sans KR subsets (Regular, Bold) packed into one blob: the content of the firmware's read-only `fonts`
 * flash partition (docs/PORTING.md, "Flash layout"). Built by tools/make_fonts_image.py, found again by
 * font_image_parse(). Plain C++, no ESP-IDF, so the format can be tested on the PC (tools/font_image/).
 *
 * Little-endian:
 *    0  "MCFT"          magic
 *    4  u32 version     1
 *    8  u32 count       2 (entry 0 = Regular, entry 1 = Bold)
 *   12  count x { u32 offset from the start of the image, u32 size }
 *   ... the font files, each starting on a 16-byte boundary */
struct FontImage
{
    const uint8_t * regular = nullptr;
    size_t regular_size     = 0;
    const uint8_t * bold    = nullptr;
    size_t bold_size        = 0;
};

/* Checks the header (magic, version, count, every entry inside `image_size`) and fills `out`; false for anything else,
 * e.g. a partition that was never written (erased flash reads as 0xFF). The pointers in `out` point into `image`. */
bool font_image_parse(const uint8_t * image, size_t image_size, FontImage & out);
