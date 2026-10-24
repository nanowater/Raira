// platform/jpeg.h on the ESP32-S3: the JPEG decoder (TJpgDec) that is already in the chip's ROM, so it costs no flash and needs no
// external component.
//
// TJpgDec decodes baseline JPEG only. The bridge re-encodes every cover as baseline (bridge/src/cover.rs), so covers that come through
// the hub decode. A progressive JPEG (what TIDAL's own CDN serves, reached only in the direct tidal-hifi mode) is refused: jd_prepare()
// answers JDR_FMT3 and the function returns false.
#include <cstring>
#include <vector>

#include "esp32s3/rom/tjpgd.h"
#include "esp_log.h"
#include "platform/jpeg.h"

namespace {

const char * TAG = "jpeg";

// working memory TJpgDec needs (tables, one MCU); 3100 bytes are the documented minimum for a baseline image, this leaves room
const size_t POOL_SIZE = 16 * 1024;

struct Ctx {
    const uint8_t * p;
    size_t left;
    RawImage * out;
    uint32_t out_w, out_h;
};

// TJpgDec asks for the next `n` bytes of the stream (buf == nullptr: skip them)
UINT input(JDEC * jd, BYTE * buf, UINT n)
{
    auto * c = static_cast<Ctx *>(jd->device);
    if(n > c->left) n = (UINT)c->left;
    if(buf) std::memcpy(buf, c->p, n);
    c->p += n;
    c->left -= n;
    return n;
}

// TJpgDec hands over one decoded rectangle as RGB888; the result is BGRX8888 (what the core wants)
UINT output(JDEC * jd, void * bitmap, JRECT * r)
{
    auto * c           = static_cast<Ctx *>(jd->device);
    const uint8_t * s  = static_cast<const uint8_t *>(bitmap);
    const uint32_t w   = (uint32_t)r->right - r->left + 1;
    const uint32_t h   = (uint32_t)r->bottom - r->top + 1;
    if(r->right >= c->out_w || r->bottom >= c->out_h) return 0; // never write outside the image: 0 stops the decoding
    for(uint32_t y = 0; y < h; y++) {
        uint8_t * d = c->out->data.data() + ((size_t)(r->top + y) * c->out_w + r->left) * 4;
        for(uint32_t x = 0; x < w; x++, s += 3, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 0xFF;
        }
    }
    return 1;
}

} // namespace

bool jpeg_decode_bgrx(const uint8_t * data, size_t size, uint32_t min_edge, RawImage & out)
{
    if(!data || size == 0) return false;

    std::vector<uint8_t> pool(POOL_SIZE);
    Ctx ctx{data, size, &out, 0, 0};
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, input, pool.data(), (UINT)pool.size(), &ctx);
    if(rc != JDR_OK) {
        ESP_LOGW(TAG, "not decodable (TJpgDec error %d; 8 = not baseline)", (int)rc);
        return false;
    }
    if(jd.width == 0 || jd.height == 0) return false;

    // Let the decoder do the cheap power-of-two downscale (1/2, 1/4, 1/8): the largest one that keeps the result at least min_edge on
    // both sides, so later resampling only shrinks. The simulator (libjpeg-turbo) picks the same.
    BYTE scale = 0;
    for(int s = 3; s >= 1; s--) {
        if((jd.width >> s) >= min_edge && (jd.height >> s) >= min_edge) {
            scale = (BYTE)s;
            break;
        }
    }
    ctx.out_w = jd.width >> scale;
    ctx.out_h = jd.height >> scale;
    if(ctx.out_w == 0 || ctx.out_h == 0) return false;

    out.data.assign((size_t)ctx.out_w * ctx.out_h * 4, 0);
    rc = jd_decomp(&jd, output, scale);
    if(rc != JDR_OK) {
        ESP_LOGW(TAG, "decoding failed (TJpgDec error %d)", (int)rc);
        out.data.clear();
        return false;
    }
    out.w = ctx.out_w;
    out.h = ctx.out_h;
    return true;
}
