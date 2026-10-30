# esp_lcd_st7701 (vendored)

- Source: Espressif `esp-iot-solution`, `components/display/lcd/esp_lcd_st7701`, version 1.1.3, commit 61935c3499f63781d1392c3bfa8be4a46eaf0bd1
  (the copy shipped in the JC4880P443C_I_W vendor examples, `common_components/espressif__esp_lcd_st7701`).
- License: Apache-2.0 (`license.txt`).
- Why copied: the board vendor's example builds against this exact copy and the panel lights up with it; whether the registry's
  release behaves the same on this panel was not compared.
- Changes: `CMakeLists.txt` without the `cmake_utilities` lines, no `idf_component.yml`, no `external_examples/`, `test_apps/`.
