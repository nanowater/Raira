#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/* Decoded cover pixels in LVGL XRGB8888 layout (bytes B,G,R,X), stride = w * 4. */
struct CoverPixels {
    std::vector<uint8_t> data;
    uint32_t w = 0;
    uint32_t h = 0;
};

/* Decode an encoded cover (JPEG) and resample it to size x size.
 * Runs off the LVGL thread so the UI only receives ready-to-draw pixels. */
/* Full-screen "ambient light" background from the cover (like YouTube's ambient mode): the cover's
 * colors, blurred, stretched over the whole screen, lifted to a visible brightness and then blended
 * onto `base_rgb` (0xRRGGBB) at a low strength so text stays readable. Output is opaque RGB565
 * (LVGL's LV_COLOR_FORMAT_RGB565, little endian), w x h pixels. */
bool cover_background(const CoverPixels & cover, uint32_t w, uint32_t h, uint32_t base_rgb, CoverPixels & out);

bool cover_decode(const std::vector<unsigned char> & encoded, uint32_t size, CoverPixels & out);
