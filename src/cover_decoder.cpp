#include "cover_decoder.h"
#include <turbojpeg.h>

bool cover_decode(const std::vector<unsigned char> & encoded, uint32_t size, CoverPixels & out)
{
    if(encoded.empty() || size == 0) return false;

    tjhandle tj = tjInitDecompress();
    if(!tj) return false;

    int w = 0, h = 0, subsamp = 0, colorspace = 0;
    if(tjDecompressHeader3(tj, encoded.data(), encoded.size(), &w, &h, &subsamp, &colorspace) != 0 || w <= 0 ||
       h <= 0) {
        tjDestroy(tj);
        return false;
    }

    /* Let libjpeg do the cheap power-of-two downscale (e.g. 1280 -> 320),
     * keeping the result at least `size` so the final resample only shrinks. */
    int n_factors                 = 0;
    tjscalingfactor * factors     = tjGetScalingFactors(&n_factors);
    int dw                        = w;
    int dh                        = h;
    for(int i = 0; i < n_factors; i++) {
        int sw = TJSCALED(w, factors[i]);
        int sh = TJSCALED(h, factors[i]);
        if(sw >= (int)size && sh >= (int)size && sw * sh < dw * dh) {
            dw = sw;
            dh = sh;
        }
    }

    std::vector<uint8_t> scaled((size_t)dw * dh * 4);
    int rc = tjDecompress2(tj, encoded.data(), encoded.size(), scaled.data(), dw, dw * 4, dh, TJPF_BGRX,
                           TJFLAG_FASTDCT);
    tjDestroy(tj);
    if(rc != 0) return false;

    /* Box-filter resample to size x size (center-crop to square first). */
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
