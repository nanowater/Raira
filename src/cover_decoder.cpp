/* Cover pixels for the UI: decoding and scaling the cover (cover_decode) and the blurred full-screen background made
 * from it (cover_background). Plain integer/float loops on purpose, no SIMD or platform code, so the same code runs on
 * the PC and on the ESP32-S3. */
#include "cover_decoder.h"
#include <algorithm>
#include <cmath>
#include "platform/jpeg.h"

bool cover_decode(const std::vector<unsigned char> & encoded, uint32_t size, CoverPixels & out)
{
    if(encoded.empty() || size == 0) return false;

    /* The decoder may already have shrunk the picture (by a power of two), but never below `size` on its short side. */
    RawImage raw;
    if(!jpeg_decode_bgrx(encoded.data(), encoded.size(), size, raw)) return false;
    const int dw                        = (int)raw.w;
    const int dh                        = (int)raw.h;
    const std::vector<uint8_t> & scaled = raw.data;

    /* Box-filter resample to size x size (center-crop to square first): every output pixel is the average of the source
     * pixels it covers, which keeps fine detail from turning into noise when the cover is shrunk. */
    int side = dw < dh ? dw : dh;
    int ox   = (dw - side) / 2;
    int oy   = (dh - side) / 2;
    out.w    = size;
    out.h    = size;
    out.data.assign((size_t)size * size * 4, 0);
    for(uint32_t y = 0; y < size; y++) {
        int y0 = oy + (int)(y * side / size);
        int y1 = oy + (int)((y + 1) * side / size);
        if(y1 <= y0) y1 = y0 + 1;
        for(uint32_t x = 0; x < size; x++) {
            int x0 = ox + (int)(x * side / size);
            int x1 = ox + (int)((x + 1) * side / size);
            if(x1 <= x0) x1 = x0 + 1;
            uint32_t acc[3] = {0, 0, 0};
            uint32_t cnt    = 0;
            for(int sy = y0; sy < y1; sy++) {
                const uint8_t * row = &scaled[((size_t)sy * dw + x0) * 4];
                for(int sx = x0; sx < x1; sx++, row += 4) {
                    acc[0] += row[0];
                    acc[1] += row[1];
                    acc[2] += row[2];
                    cnt++;
                }
            }
            uint8_t * px = &out.data[((size_t)y * size + x) * 4];
            px[0]        = (uint8_t)(acc[0] / cnt);
            px[1]        = (uint8_t)(acc[1] / cnt);
            px[2]        = (uint8_t)(acc[2] / cnt);
            px[3]        = 0xFF;
        }
    }
    return true;
}

/* Steps: average the cover into a 16x16 grid of colors and smooth it; lift dark covers and saturate a little; stretch
 * the grid over w x h with bilinear interpolation; blend it onto the base color, strongest in the middle; write RGB565
 * with dithering. The cover has to be square (what cover_decode gives). */
bool cover_background(const CoverPixels & cover, uint32_t w, uint32_t h, uint32_t base_rgb, CoverPixels & out)
{
    const uint32_t cs = cover.w;
    if(cs == 0 || cover.h != cs || cover.data.size() < (size_t)cs * cs * 4 || w == 0 || h == 0) return false;

    /* Blur: average the cover into a coarse grid, then smooth it, so only broad color areas remain. */
    const int G    = 16;
    const int cell = (int)cs / G; /* cs = 250 -> 15 px cells, the remainder is ignored */
    if(cell < 1) return false;
    std::vector<float> grid((size_t)G * G * 3, 0.f), tmp((size_t)G * G * 3, 0.f);
    for(int gy = 0; gy < G; gy++)
        for(int gx = 0; gx < G; gx++) {
            double acc[3] = {0, 0, 0};
            for(int y = 0; y < cell; y++)
                for(int x = 0; x < cell; x++) {
                    const uint8_t * px = &cover.data[(((size_t)gy * cell + y) * cs + (size_t)gx * cell + x) * 4];
                    acc[0] += px[0];
                    acc[1] += px[1];
                    acc[2] += px[2];
                }
            for(int c = 0; c < 3; c++) grid[((size_t)gy * G + gx) * 3 + c] = (float)(acc[c] / (cell * cell));
        }
    for(int pass = 0; pass < 2; pass++) { /* 3x3 box blur, edges clamped */
        for(int y = 0; y < G; y++)
            for(int x = 0; x < G; x++)
                for(int c = 0; c < 3; c++) {
                    float sum = 0;
                    for(int dy = -1; dy <= 1; dy++)
                        for(int dx = -1; dx <= 1; dx++) {
                            int yy = std::min(std::max(y + dy, 0), G - 1), xx = std::min(std::max(x + dx, 0), G - 1);
                            sum += grid[((size_t)yy * G + xx) * 3 + c];
                        }
                    tmp[((size_t)y * G + x) * 3 + c] = sum / 9.f;
                }
        grid.swap(tmp);
    }

    /* Dark covers would give no visible effect: lift the overall brightness toward a target (never dim
     * a bright cover) and add some saturation. Grid order is B,G,R. */
    double lum = 0;
    for(size_t i = 0; i < (size_t)G * G; i++)
        lum += 0.299 * grid[i * 3 + 2] + 0.587 * grid[i * 3 + 1] + 0.114 * grid[i * 3 + 0];
    lum /= (double)G * G;
    const float gain = (float)std::min(std::max(150.0 / std::max(lum, 1.0), 1.0), 3.0);
    for(size_t i = 0; i < (size_t)G * G; i++) {
        float b = grid[i * 3 + 0] * gain, g = grid[i * 3 + 1] * gain, r = grid[i * 3 + 2] * gain;
        float l         = 0.299f * r + 0.587f * g + 0.114f * b;
        const float sat = 1.25f;
        grid[i * 3 + 0] = std::min(255.f, std::max(0.f, l + (b - l) * sat));
        grid[i * 3 + 1] = std::min(255.f, std::max(0.f, l + (g - l) * sat));
        grid[i * 3 + 2] = std::min(255.f, std::max(0.f, l + (r - l) * sat));
    }

    /* Per output pixel: bilinear lookup in the grid (the four nearest cells), then the blend onto the base color. */
    out.w = w;
    out.h = h;
    out.data.assign((size_t)w * h * 2, 0);

    const float strength = 0.34f; /* how much of the cover color ends up in the background */
    const float base_r = (float)((base_rgb >> 16) & 0xFF), base_g = (float)((base_rgb >> 8) & 0xFF),
                base_b = (float)(base_rgb & 0xFF);
    for(uint32_t y = 0; y < h; y++) {
        float gy = ((float)y / (float)(h - 1)) * (G - 1);
        int y0 = (int)gy, y1 = std::min(y0 + 1, G - 1);
        float fy = gy - y0;
        for(uint32_t x = 0; x < w; x++) {
            float gx = ((float)x / (float)(w - 1)) * (G - 1);
            int x0 = (int)gx, x1 = std::min(x0 + 1, G - 1);
            float fx = gx - x0;
            float col[3];
            for(int c = 0; c < 3; c++)
                col[c] = grid[((size_t)y0 * G + x0) * 3 + c] * (1 - fx) * (1 - fy) +
                         grid[((size_t)y0 * G + x1) * 3 + c] * fx * (1 - fy) +
                         grid[((size_t)y1 * G + x0) * 3 + c] * (1 - fx) * fy +
                         grid[((size_t)y1 * G + x1) * 3 + c] * fx * fy;
            /* gentle vignette so the edges settle back toward the base color */
            float nx = (float)x / (float)w - 0.5f, ny = (float)y / (float)h - 0.5f;
            float vig = 1.f - 0.8f * (nx * nx + ny * ny);
            float a   = strength * std::max(vig, 0.f);
            float r   = base_r * (1 - a) + col[2] * a;
            float g   = base_g * (1 - a) + col[1] * a;
            float b   = base_b * (1 - a) + col[0] * a;
            /* RGB565 has only 32/64 levels, which shows as contour bands on a smooth dark gradient:
             * ordered (Bayer 4x4) dithering turns the banding into fine, invisible noise. */
            static const float BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
            const float t                = (BAYER[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.f;
            int r5                       = std::min(31, (int)(r * 31.f / 255.f + t));
            int g6                       = std::min(63, (int)(g * 63.f / 255.f + t));
            int b5                       = std::min(31, (int)(b * 31.f / 255.f + t));
            uint16_t px                  = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            uint8_t * o                  = &out.data[((size_t)y * w + x) * 2];
            o[0]                         = (uint8_t)(px & 0xFF);
            o[1]                         = (uint8_t)(px >> 8);
        }
    }
    return true;
}
