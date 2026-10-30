# esp_lcd_st7701 1.1.3 (Espressif, Apache-2.0), copied from the vendor's examples for the JC4880P443C_I_W: it holds the init
# sequence that lights up this board's panel (see VENDORED.md). The two lines of the original that pulled in `cmake_utilities`
# (include(package_manager), cu_pkg_define_version) are replaced by the three version defines below; the C files are unchanged.
idf_component_register(SRCS "esp_lcd_st7701.c" "esp_lcd_st7701_rgb.c" "esp_lcd_st7701_mipi.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES "esp_lcd" "driver")

# what cu_pkg_define_version() defined (version 1.1.3)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP_LCD_ST7701_VER_MAJOR=1 ESP_LCD_ST7701_VER_MINOR=1 ESP_LCD_ST7701_VER_PATCH=3)
