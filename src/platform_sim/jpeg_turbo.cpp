/* jpeg_decode_bgrx() on libjpeg-turbo (TurboJPEG) for the simulator. */
#include "platform/jpeg.h"
#include <turbojpeg.h>

bool jpeg_decode_bgrx(const uint8_t * data, size_t size, uint32_t min_edge, RawImage & out)
{
    if(!data || size == 0) return false;

    tjhandle tj = tjInitDecompress();
    if(!tj) return false;

    int w = 0, h = 0, subsamp = 0, colorspace = 0;
    if(tjDecompressHeader3(tj, data, size, &w, &h, &subsamp, &colorspace) != 0 || w <= 0 || h <= 0) {
        tjDestroy(tj);
        return false;
    }

    /* Let libjpeg do the cheap power-of-two downscale (e.g. 1280 -> 320), keeping the result at least
     * min_edge so later resampling only shrinks. */
    int n_factors             = 0;
    tjscalingfactor * factors = tjGetScalingFactors(&n_factors);
    int dw                    = w;
    int dh                    = h;
    for(int i = 0; i < n_factors; i++) {
        int sw = TJSCALED(w, factors[i]);
        int sh = TJSCALED(h, factors[i]);
        if(sw >= (int)min_edge && sh >= (int)min_edge && sw * sh < dw * dh) {
            dw = sw;
            dh = sh;
        }
    }

    out.data.assign((size_t)dw * dh * 4, 0);
    int rc = tjDecompress2(tj, data, size, out.data.data(), dw, dw * 4, dh, TJPF_BGRX, TJFLAG_FASTDCT);
    tjDestroy(tj);
    if(rc != 0) return false;
    out.w = (uint32_t)dw;
    out.h = (uint32_t)dh;
    return true;
}
