// What differs between the two boards: the JC8048W550 (ESP32-S3, RGB panel) and the JC4880P443C_I_W (ESP32-P4, MIPI-DSI panel).
// board_s3.cpp / board_p4.cpp implement it; CMakeLists.txt compiles the one that matches the chip.
#pragma once

// Brings the board up as far as it is written: panel, backlight and (later) touch. Returns false when a step fails.
bool board_init();
