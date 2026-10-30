// board.h for the JC4880P443C_I_W (ESP32-P4 + ESP32-C6, ST7701 480x800 panel over MIPI-DSI, GT911 touch).
//
// Stage 1: panel and backlight, ending in colour bars. The calls, in order (values from the vendor's example,
// docs/PORTING.md):
//   1. DSI PHY power:  esp_ldo_acquire_channel()        LDO channel 3, 2500 mV
//   2. DSI bus:        esp_lcd_new_dsi_bus()            bus 0, 2 lanes, 750 Mbit/s, phy_clk_src 0
//   3. command path:   esp_lcd_new_panel_io_dbi()       virtual channel 0, 8-bit commands and parameters
//   4. DPI timing:     esp_lcd_dpi_panel_config_t       480x800, 28 MHz, RGB565, h porch 42/12/42, v porch 8/2/166
//   5. panel:          esp_lcd_new_panel_st7701() + reset (GPIO 5) + init
//   6. backlight:      ledc_timer_config() + ledc_channel_config()   GPIO 23, 5 kHz, 10 bit
//   7. test picture:   esp_lcd_dpi_panel_get_frame_buffer(), write colour bars
#include "board.h"
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_dev.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include <thread>
#include <chrono>
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <lvgl.h>

namespace {
const char * TAG               = "board";
constexpr uint32_t PANEL_W     = 480; // native panel size (portrait)
constexpr uint32_t PANEL_H     = 800;
ppa_client_handle_t s_ppa      = nullptr;
void * s_fb                    = nullptr; // the DPI frame buffer
esp_lcd_touch_handle_t s_touch = nullptr;

// PPA finished a strip: LVGL may reuse the buffer (runs in the interrupt: only sets a flag)
bool ppa_done_cb(ppa_client_handle_t, ppa_event_data_t *, void * user_data)
{
    lv_display_flush_ready(static_cast<lv_display_t *>(user_data));
    return false; // no task switch needed
}

// LVGL finished a strip (landscape 800x480); rotate it into the portrait frame buffer
void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    const uint32_t w = lv_area_get_width(area);
    const uint32_t h = lv_area_get_height(area);

    ppa_srm_oper_config_t srm = {};
    srm.in.buffer             = px_map;
    srm.in.pic_w              = w;
    srm.in.pic_h              = h;
    srm.in.block_w            = w;
    srm.in.block_h            = h;
    srm.in.block_offset_x     = 0;
    srm.in.block_offset_y     = 0;
    srm.in.srm_cm             = PPA_SRM_COLOR_MODE_RGB565;

    srm.out.buffer         = s_fb;
    srm.out.buffer_size    = PANEL_W * PANEL_H * sizeof(uint16_t);
    srm.out.pic_w          = PANEL_W;
    srm.out.pic_h          = PANEL_H;
    srm.out.block_offset_x = static_cast<uint32_t>(area->y1);
    srm.out.block_offset_y = PANEL_H - 1 - static_cast<uint32_t>(area->x2);
    srm.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    srm.scale_x        = 1.0f;
    srm.scale_y        = 1.0f;
    srm.mode           = PPA_TRANS_MODE_NON_BLOCKING;
    srm.user_data      = disp;

    if(ppa_do_scale_rotate_mirror(s_ppa, &srm) != ESP_OK) {
        ESP_LOGE(TAG, "PPA rotation failed");
        lv_display_flush_ready(disp); // nothing will call back: do not leave LVGL waiting
    }
}

// LVGL asks for the finger position (landscape); the chip reports portrait
void touch_read_cb(lv_indev_t *, lv_indev_data_t * data)
{
    if(!s_touch) { // the controller did not answer at start-up: no touch, the screen still works
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    bool ok =
        esp_lcd_touch_read_data(s_touch) == ESP_OK && esp_lcd_touch_get_data(s_touch, pt, &n, 1) == ESP_OK && n > 0;
    if(ok) {
        data->point.x = PANEL_H - 1 - pt[0].y;
        data->point.y = pt[0].x;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
} // namespace

bool board_init()
{
    static esp_ldo_channel_handle_t phy_pwr = nullptr;
    esp_ldo_channel_config_t ldo_cfg        = {
        .chan_id                 = 3,
        .voltage_mv              = 2500,
        .voltage_stable_delay_us = 0,
        .flags                   = {},
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr));

    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = 2,
        .phy_clk_src        = {},
        .lane_bit_rate_mbps = 750,
        .flags              = {},
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_dbi_io_config_t dbi_cfg  = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 28,
        .in_color_format    = LCD_COLOR_FMT_RGB565,
        .out_color_format   = LCD_COLOR_FMT_RGB565,
        .num_fbs            = 1,
        .video_timing =
            {
                .h_size            = 480,
                .v_size            = 800,
                .hsync_pulse_width = 12,
                .hsync_back_porch  = 42,
                .hsync_front_porch = 42,
                .vsync_pulse_width = 2,
                .vsync_back_porch  = 8,
                .vsync_front_porch = 166,
            },
        .flags = {},
    };

    st7701_vendor_config_t vendor_cfg = {
        .init_cmds      = nullptr, // nullptr: the driver's default  this panel up)
        .init_cmds_size = 0,
        .mipi_config =
            {
                .dsi_bus    = dsi_bus,
                .dpi_config = &dpi_cfg,
            },
        .flags =
            {
                .use_mipi_interface = 1,
                .mirror_by_cmd      = 0,
                .auto_del_panel_io  = 0, // see the note below; 3 flags are in a union, only the first is named here
            },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .reset_gpio_num = GPIO_NUM_5,
        .vendor_config  = &vendor_cfg,
        .flags          = {},
    };

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(dbi_io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    ledc_timer_config_t bl_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));

    ledc_channel_config_t bl_ch = {};
    bl_ch.gpio_num              = 23;
    bl_ch.speed_mode            = LEDC_LOW_SPEED_MODE;
    bl_ch.channel               = LEDC_CHANNEL_0;
    bl_ch.timer_sel             = LEDC_TIMER_1;
    bl_ch.duty                  = 1023; // full brightness (2^10 - 1)
    bl_ch.hpoint                = 0;
    bl_ch.sleep_mode            = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    void * fb = nullptr;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb));

    s_fb = fb;

    // I2C bus: touch (and later the audio codec) share it
    i2c_master_bus_handle_t i2c_bus      = nullptr;
    i2c_master_bus_config_t i2c_cfg      = {};
    i2c_cfg.i2c_port                     = I2C_NUM_0;
    i2c_cfg.sda_io_num                   = GPIO_NUM_7;
    i2c_cfg.scl_io_num                   = GPIO_NUM_8;
    i2c_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt            = 7;
    i2c_cfg.flags.enable_internal_pullup = 1;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    // panel IO for the GT911 (what ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG() fills in)
    esp_lcd_panel_io_handle_t touch_io   = nullptr;
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr                      = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    io_cfg.scl_speed_hz                  = 100000;
    io_cfg.control_phase_bytes           = 1;
    io_cfg.dc_bit_offset                 = 0;
    io_cfg.lcd_cmd_bits                  = 16;
    io_cfg.flags.disable_control_phase   = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &touch_io));

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max                  = 480;
    tp_cfg.y_max                  = 800;
    tp_cfg.rst_gpio_num           = GPIO_NUM_NC;
    tp_cfg.int_gpio_num           = GPIO_NUM_NC;
    // The GT911 may not answer right after a reset of the ESP32 (seen once): try a few times, and go on without touch
    // if it still does not (the screen is usable, the device must not die for it).
    for(int attempt = 1; attempt <= 3 && !s_touch; attempt++) {
        if(esp_lcd_touch_new_i2c_gt911(touch_io, &tp_cfg, &s_touch) != ESP_OK) {
            s_touch = nullptr;
            ESP_LOGW(TAG, "GT911 did not answer (attempt %d of 3)", attempt);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if(!s_touch) ESP_LOGE(TAG, "no touch: the GT911 does not answer");

    ppa_client_config_t ppa_cfg   = {};
    ppa_cfg.oper_type             = PPA_OPERATION_SRM;
    ppa_cfg.max_pending_trans_num = 2; // the next strip may arrive before the finished one is recycled
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_ppa));
    ppa_event_callbacks_t ppa_cbs = {};
    ppa_cbs.on_trans_done         = ppa_done_cb;
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_ppa, &ppa_cbs));

    lv_tick_set_cb([]() -> uint32_t { return static_cast<uint32_t>(esp_timer_get_time() / 1000); });

    lv_display_t * disp = lv_display_create(800, 480);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    const uint32_t buf_size = 800 * 20 * sizeof(uint16_t); // two strips of 20 lines: 64 KB of the internal RAM
    void * buf1             = heap_caps_aligned_alloc(128, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void * buf2             = heap_caps_aligned_alloc(128, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);

    return true;
}
