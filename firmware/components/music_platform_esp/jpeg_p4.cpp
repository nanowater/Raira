// platform/jpeg.h on the ESP32-P4: the chip's hardware JPEG decoder (esp_driver_jpeg).
#include <cstring>
#include <memory>
#include <mutex>

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "platform/jpeg.h"

namespace {

const char * TAG = "jpeg";

std::mutex s_lock; // one decode at a time (the poller and the browse thread both decode)
jpeg_decoder_handle_t s_engine = nullptr;

bool engine_ready()
{
    if(s_engine) return true;
    jpeg_decode_engine_cfg_t cfg = {};
    cfg.timeout_ms               = 1000;
    if(jpeg_new_decoder_engine(&cfg, &s_engine) != ESP_OK) {
        ESP_LOGE(TAG, "cannot create the JPEG decoder engine");
        s_engine = nullptr;
        return false;
    }
    return true;
}

struct HeapDeleter
{
    void operator()(uint8_t * p) const
    {
        heap_caps_free(p);
    }
};
using HwBuf = std::unique_ptr<uint8_t, HeapDeleter>;

// a buffer the hardware can read (input) or write (output): right alignment and memory type
HwBuf alloc_hw(size_t size, jpeg_dec_buffer_alloc_direction_t dir, size_t & got)
{
    jpeg_decode_memory_alloc_cfg_t cfg = {};
    cfg.buffer_direction               = dir;
    return HwBuf(static_cast<uint8_t *>(jpeg_alloc_decoder_mem(size, &cfg, &got)));
}

} // namespace

bool jpeg_decode_bgrx(const uint8_t * data, size_t size, uint32_t, RawImage & out)
{
    if(!data || size == 0) return false;
    std::lock_guard<std::mutex> lock(s_lock);
    if(!engine_ready()) return false;

    jpeg_decode_picture_info_t info = {};
    if(jpeg_decoder_get_info(data, static_cast<uint32_t>(size), &info) != ESP_OK || info.width == 0 ||
       info.height == 0) {
        ESP_LOGW(TAG, "not a JPEG the hardware can read");
        return false;
    }
    // For YUV420/422 the decoder writes whole 16x16 blocks: the buffer must hold the padded size
    const uint32_t pad_w = (info.width + 15) & ~15u;
    const uint32_t pad_h = (info.height + 15) & ~15u;

    size_t in_size = 0, out_size_alloc = 0;
    HwBuf in  = alloc_hw(size, JPEG_DEC_ALLOC_INPUT_BUFFER, in_size);
    HwBuf buf = alloc_hw(static_cast<size_t>(pad_w) * pad_h * 3, JPEG_DEC_ALLOC_OUTPUT_BUFFER, out_size_alloc);
    if(!in || !buf) {
        ESP_LOGW(TAG, "no memory for a %ux%u picture", (unsigned)info.width, (unsigned)info.height);
        return false;
    }
    std::memcpy(in.get(), data, size);

    jpeg_decode_cfg_t cfg = {};
    cfg.output_format     = JPEG_DECODE_OUT_FORMAT_RGB888;
    cfg.rgb_order         = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    cfg.conv_std          = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t produced     = 0;
    if(jpeg_decoder_process(s_engine, &cfg, in.get(), static_cast<uint32_t>(size), buf.get(),
                            static_cast<uint32_t>(out_size_alloc), &produced) != ESP_OK) {
        ESP_LOGW(TAG, "decoding failed (progressive or unsupported JPEG?)");
        return false;
    }

    // 3 bytes per pixel (B, G, R) with padded rows -> BGRX8888 of the visible size
    out.data.assign(static_cast<size_t>(info.width) * info.height * 4, 0xFF);
    for(uint32_t y = 0; y < info.height; y++) {
        const uint8_t * s = buf.get() + static_cast<size_t>(y) * pad_w * 3;
        uint8_t * d       = out.data.data() + static_cast<size_t>(y) * info.width * 4;
        for(uint32_t x = 0; x < info.width; x++, s += 3, d += 4) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    out.w = info.width;
    out.h = info.height;
    return true;
}
