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
bool cover_decode(const std::vector<unsigned char> & encoded, uint32_t size, CoverPixels & out);
