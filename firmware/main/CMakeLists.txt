# The board file depends on the chip (board.h).
if(IDF_TARGET STREQUAL "esp32p4")
    set(BOARD_SRC board_p4.cpp)
    set(BOARD_REQUIRES esp_lcd esp_lcd_st7701 esp_driver_ledc esp_driver_i2c esp_hw_support esp_mm esp_driver_ppa espressif__esp_lcd_touch_gt911)
else()
    set(BOARD_SRC board_s3.cpp)
    set(BOARD_REQUIRES "")
endif()

idf_component_register(SRCS "main.cpp" ${BOARD_SRC}
                       INCLUDE_DIRS "."
                       REQUIRES lvgl esp_psram esp_timer pthread music_core music_platform_esp nvs_flash ${BOARD_REQUIRES})

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
