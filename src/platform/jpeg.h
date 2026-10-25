#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

/* JPEG decoding the app core needs. Simulator: libjpeg-turbo (src/platform_sim); ESP32: an ESP JPEG
 * decoder. Note TIDAL serves progressive JPEGs, which some embedded decoders do not support. */
struct RawImage
{
    std::vector<uint8_t> data; /* BGRX8888: bytes B,G,R,X per pixel, stride = w * 4 */
    uint32_t w = 0;
    uint32_t h = 0;
};

/* Decode `data` to BGRX8888. The decoder may downscale by a power of two for speed, but the result must
 * stay at least `min_edge` pixels on its shorter side (when the source is that large). */
bool jpeg_decode_bgrx(const uint8_t * data, size_t size, uint32_t min_edge, RawImage & out);
