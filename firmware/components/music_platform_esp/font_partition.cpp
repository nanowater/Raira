#include <cstddef>
#include "esp_partition.h"
#include "esp_log.h"
#include <mutex>
#include "fonts/font_image.h"

static const char * TAG = "fonts";
static FontImage s_fonts;
static bool s_ok = false;
static std::once_flag s_once;

static const unsigned char * map_partition(size_t * size)
{
    const esp_partition_t * partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), "fonts");

    if(partition == nullptr) {
        ESP_LOGE(TAG, "fonts partition not found");
        return nullptr;
    }

    const void * ptr = nullptr;
    esp_partition_mmap_handle_t handle;

    esp_err_t err = esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);

    if(err != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return nullptr;
    }
    ESP_LOGI(TAG, "fonts partition at 0x%x, %u bytes", static_cast<unsigned>(partition->address),
             static_cast<unsigned>(partition->size));

    *size = partition->size;
    return static_cast<const unsigned char *>(ptr);
}

static void load()
{
    size_t size               = 0;
    const unsigned char * ptr = map_partition(&size);
    if(ptr != nullptr) {
        if(font_image_parse(ptr, size, s_fonts)) {
            s_ok = true;
        } else {
            ESP_LOGE(TAG, "fonts partition holds no font image");
        }
    }
}

extern "C" const unsigned char * noto_sans_kr_get(size_t * size)
{
    std::call_once(s_once, load);
    if(!s_ok) {
        *size = 0;
        return nullptr;
    }
    *size = s_fonts.regular_size;
    return s_fonts.regular;
}

extern "C" const unsigned char * noto_sans_kr_bold_get(size_t * size)
{
    std::call_once(s_once, load);
    if(!s_ok) {
        *size = 0;
        return nullptr;
    }
    *size = s_fonts.bold_size;
    return s_fonts.bold;
}
