// platform/jpeg.h on the ESP32-P4: NOT WRITTEN YET. The P4 has no TJpgDec in its ROM (jpeg_esp.cpp is the S3's); the hardware
// JPEG codec (esp_driver_jpeg) is the planned replacement. Until then this only lets the firmware link: no cover is decoded.
#include "esp_log.h"
#include "platform/jpeg.h"

bool jpeg_decode_bgrx(const uint8_t *, size_t, uint32_t, RawImage &)
{
    ESP_LOGW("jpeg", "no JPEG decoder on this chip yet");
    return false;
}
