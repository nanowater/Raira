#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>

#if LV_BUILD_DEMOS
    #include <lv_demos.h>
#endif /*LV_BUILD_DEMOS*/

#if LV_BUILD_EXAMPLES
    #include <lv_examples.h>
#endif /*LV_BUILD_EXAMPLES*/

#ifdef CONFIG_LV_USE_DEMO_TRUCK
    #include <lv_demo_truck.h>
#endif /*CONFIG_LV_USE_DEMO_TRUCK*/

#include "lib/driver_backends.h"
#include "lib/simulator_util.h"
#include "lib/simulator_settings.h"
#include "music_app.h"
#include "media_poller.h"
#include "tidal_client.h"

/* Internal functions */
static void configure_simulator(int argc, char ** argv);
static void install_volume_input(void);
static TidalConfig tidal_cfg;
static void print_lvgl_version(void);
static void print_usage(void);
static void show_demo(void);

/* contains the name of the selected backend if user
 * has specified one on the command line */
static char * selected_backend;

/* when true, run the LVGL demo instead of the app UI */
static bool show_demo_screen = false;

/* settings is declared extern "C" in lib/simulator_settings.h */


/**
 * @brief Print LVGL version
 */
static void print_lvgl_version(void)
{
    fprintf(stdout, "%d.%d.%d-%s\n",
            LVGL_VERSION_MAJOR,
            LVGL_VERSION_MINOR,
            LVGL_VERSION_PATCH,
            LVGL_VERSION_INFO);
}

/**
 * @brief Print usage information
 */
static void print_usage(void)
{
    fprintf(stdout,
            "\nlvglsim [-V] [-B] [-D] [-f] [-m] [-s host[:port]] [-b backend_name] [-W window_width] [-H window_height] [-R rotation]\n\n");
    fprintf(stdout, "-V print LVGL version\n");
    fprintf(stdout, "-B list supported backends\n");
    fprintf(stdout, "-D run the LVGL demo instead of the app UI\n");
    fprintf(stdout, "-f fullscreen\n");
    fprintf(stdout, "-m maximize\n");
    fprintf(stdout, "-s tidal-hifi host[:port] (default localhost:47836, env TIDAL_HIFI_HOST)\n");
}

/**
 * @brief Run the LVGL demo screen (kept for reference)
 */
static void show_demo(void)
{
#ifdef CONFIG_LV_USE_DEMO_TRUCK
    const char * assets_path = getenv("LV_LINUX_3D_PATH");
    if(!assets_path) {
        assets_path = "3d";
    }
    lv_demo_truck(assets_path);
#elif LV_USE_DEMO_GLTF
    const char * model_path = getenv("LV_LINUX_3D_MODEL_PATH");
    if(!model_path) {
        model_path = "3d/lvgl_logo.glb";
    }
    lv_demo_gltf(model_path);
#elif LV_USE_DEMO_BENCHMARK
    /*Run benchmark*/
    lv_demo_benchmark();
#elif LV_USE_DEMO_WIDGETS
    /*Create a Demo*/
    lv_demo_widgets();
    lv_demo_widgets_start_slideshow();
#else
    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_label_set_text_static(label, "Demos not enabled, create your own ui in `src/main.cpp`");
    lv_obj_center(label);
#endif /*LV_BUILD_DEMOS*/
}

/**
 * @brief Configure simulator
 * @description process arguments received by the program to select
 * appropriate options
 * @param argc the count of arguments in argv
 * @param argv The arguments
 */
static void configure_simulator(int argc, char ** argv)
{
    int opt = 0;

    selected_backend = NULL;
    driver_backends_register();

    if(const char * env_host = getenv("TIDAL_HIFI_HOST")) {
        if(!tidal_cfg.parse(env_host)) die("Invalid TIDAL_HIFI_HOST '%s'\n", env_host);
    }

    const char * env_w = getenv("LV_SIM_WINDOW_WIDTH");
    const char * env_h = getenv("LV_SIM_WINDOW_HEIGHT");
    /* Default values */
    settings.window_width = atoi(env_w ? env_w : "800");
    settings.window_height = atoi(env_h ? env_h : "480");

    /* Parse the command-line options. */
    while((opt = getopt(argc, argv, "b:fmW:H:R:s:BVDh")) != -1) {
        switch(opt) {
            case 'h':
                print_usage();
                exit(EXIT_SUCCESS);
                break;
            case 'V':
                print_lvgl_version();
                exit(EXIT_SUCCESS);
                break;
            case 'B':
                driver_backends_print_supported();
                exit(EXIT_SUCCESS);
                break;
            case 's':
                if(!tidal_cfg.parse(optarg)) die("Invalid -s value '%s' (expected host[:port])\n", optarg);
                break;
            case 'D':
                show_demo_screen = true;
                break;
            case 'b':
                if(driver_backends_is_supported(optarg) == 0) {
                    die("error no such backend: %s\n", optarg);
                }
                selected_backend = strdup(optarg);
                break;
            case 'f':
                settings.fullscreen = true;
                break;
            case 'm':
                settings.maximize = true;
                break;
            case 'W':
                settings.window_width = atoi(optarg);
                break;
            case 'H':
                settings.window_height = atoi(optarg);
                break;
            case 'R':
                switch(atoi(optarg)) {
                    case 0:
                        settings.rotation = LV_DISPLAY_ROTATION_0;
                        break;
                    case 90:
                        settings.rotation = LV_DISPLAY_ROTATION_90;
                        break;
                    case 180:
                        settings.rotation = LV_DISPLAY_ROTATION_180;
                        break;
                    case 270:
                        settings.rotation = LV_DISPLAY_ROTATION_270;
                        break;
                    default:
                        LV_LOG_WARN("Invalid rotation angle. Valid angles are {0, 90, 180, 270}");
                        break;
                }
                break;
            case ':':
                print_usage();
                die("Option -%c requires an argument.\n", optopt);
                break;
            case '?':
                print_usage();
                die("Unknown option -%c.\n", optopt);
        }
    }
}

/* Simulator stand-in for the rotary encoder: mouse wheel (scroll up = louder) and the Up/Down
 * arrow keys step the volume. The wheel is an LVGL encoder indev, which only delivers LEFT/RIGHT
 * keys to a focused object of its group in edit mode, so give the screen that role. The real
 * device will call media_poller_volume_step() straight from its encoder driver. */
static void volume_key_cb(lv_event_t * e)
{
    switch(lv_event_get_key(e)) {
        case LV_KEY_UP:
        case LV_KEY_LEFT: /* wheel up */
            media_poller_volume_step(+1);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT: /* wheel down */
            media_poller_volume_step(-1);
            break;
        default:
            break;
    }
}

static void install_volume_input(void)
{
    lv_group_t * g = lv_group_create();
    lv_group_set_default(g);
    for(lv_indev_t * i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        lv_indev_type_t t = lv_indev_get_type(i);
        if(t == LV_INDEV_TYPE_ENCODER || t == LV_INDEV_TYPE_KEYPAD) lv_indev_set_group(i, g);
    }
    lv_obj_t * scr = lv_screen_active();
    lv_group_add_obj(g, scr);
    lv_group_set_editing(g, true);
    /* no focus outline around the whole screen */
    lv_obj_set_style_outline_width(scr, 0, (lv_style_selector_t)LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    lv_obj_add_event_cb(scr, volume_key_cb, LV_EVENT_KEY, NULL);
}

/**
 * @brief entry point
 * @description start the app UI (or the demo with -D)
 * @param argc the count of arguments in argv
 * @param argv The arguments
 */
int main(int argc, char ** argv)
{

    configure_simulator(argc, argv);

    /* Initialize LVGL. */
    lv_init();

    /* Initialize the configured backend */
    if(driver_backends_init_backend(selected_backend) == -1) {
        die("Failed to initialize display backend");
    }
    if(settings.rotation) {
#if LV_USE_DRAW_NANOVG && LV_DRAW_TRANSFORM_USE_MATRIX
        lv_display_set_matrix_rotation(NULL, true);
#endif
        lv_display_set_rotation(NULL, settings.rotation);
    }

    /* Enable for EVDEV support */
#if LV_USE_EVDEV
    if(driver_backends_init_backend("EVDEV") == -1) {
        die("Failed to initialize evdev");
    }
#endif

    if(show_demo_screen) {
        show_demo();
    }
    else {
        music_app_start(std::make_unique<TidalClient>(tidal_cfg));
        install_volume_input();
    }

    while(1) {
        uint32_t ms = lv_timer_handler();
        if(ms == LV_NO_TIMER_READY) {
            ms = LV_DEF_REFR_PERIOD;
        }
        usleep(ms * 1000);
    }

    return 0;
}
