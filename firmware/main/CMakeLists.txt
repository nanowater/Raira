# The board: JC4880P443C_I_W (ESP32-P4), see board.h.
set(BOARD_SRC board_p4.cpp)
set(BOARD_REQUIRES esp_lcd esp_lcd_st7701 esp_driver_ledc esp_driver_i2c esp_hw_support esp_mm esp_driver_ppa espressif__esp_lcd_touch_gt911)

idf_component_register(SRCS "main.cpp" "encoder.cpp" ${BOARD_SRC}
                       INCLUDE_DIRS "."
                       REQUIRES lvgl esp_driver_pcnt esp_driver_gpio esp_psram esp_timer pthread music_core music_platform_esp nvs_flash ${BOARD_REQUIRES})

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
