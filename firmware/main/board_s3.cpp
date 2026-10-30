// board.h for the JC8048W550. Not carried over yet: the RGB panel / GT911 test code from the first board is in `git stash` ("S3 board
// TEMP test code"; it holds WiFi credentials, never commit it) until a working S3 board is available again.
#include "board.h"

#include "esp_log.h"

bool board_init()
{
    ESP_LOGW("board", "JC8048W550: no display code yet");
    return false;
}
