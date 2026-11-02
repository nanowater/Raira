// What the board provides: the JC4880P443C_I_W (ESP32-P4, MIPI-DSI panel). board_p4.cpp implements it.
#pragma once

// Brings the board up as far as it is written: panel, backlight and (later) touch. Returns false when a step fails.
bool board_init();

// Backlight in percent (5..100; 100 = full). Called from the LVGL thread. The board keeps no state: the app applies the
// saved value at start (MusicAppDeps::set_brightness).
void board_set_backlight(int percent);
