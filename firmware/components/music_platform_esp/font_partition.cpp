#include <cstddef>
#include "esp_partition.h"
#include "esp_log.h"

static const char * TAG = "fonts";

static void log_partition()
{
    const esp_partition_t * partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), "fonts");

    if(partition == nullptr) {
        ESP_LOGE(TAG, "fonts partition not found");
        return;
    }
    ESP_LOGI(TAG, "fonts partition at 0x%x, %u bytes", static_cast<unsigned>(partition->address),
             static_cast<unsigned>(partition->size));
}

extern "C" const unsigned char * noto_sans_kr_get(size_t * size)
{
    log_partition();
    *size = 0;
    return nullptr;
}

extern "C" const unsigned char * noto_sans_kr_bold_get(size_t * size)
{
    *size = 0;
    return nullptr;
}
