#include <unistd.h>
#include <pthread.h>
#include <signal.h>
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
#include "fake_browse.h"
#include <nlohmann/json.hpp>
#include <random>
#include "bridge_client.h"
#include "hub_source.h"
#include "platform/mdns.h"
#include "tidal_client.h"
#include "platform/settings.h"
#include "platform/wifi.h"

/* Internal functions */
static void configure_simulator(int argc, char ** argv);
static void install_volume_input(void);
static TidalConfig tidal_cfg;
static volatile sig_atomic_t g_quit = 0; /* set by SIGINT/SIGTERM: leave the main loop so the hub says goodbye on mDNS */
static void on_quit_signal(int) { g_quit = 1; }
static std::unique_ptr<MdnsAdvertiser> g_mdns; /* announces the hub for the whole run */
static int hub_port = 0; /* -S <port>: be the hub, sources (the PC bridge) connect to this port */
static std::string cli_token;  /* -T or MUSIC_BRIDGE_TOKEN: the host bridge's access token */
static std::string cli_server; /* the raw -s value: the bridge has another default port than tidal-hifi */
static bool use_bridge = false; /* -P: talk to the host bridge instead of tidal-hifi */
static bool fake_browse = false; /* -F: canned playlists/queue behind the browse button */
static bool host_from_cli = false; /* -s or TIDAL_HIFI_HOST given: wins over the saved setting */
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
            "\nlvglsim [-V] [-B] [-D] [-F] [-P] [-f] [-m] [-s host[:port]] [-T token] [-S port] [-b backend_name] [-W window_width] [-H window_height] [-R rotation]\n\n");
    fprintf(stdout, "-V print LVGL version\n");
    fprintf(stdout, "-B list supported backends\n");
    fprintf(stdout, "-D run the LVGL demo instead of the app UI\n");
    fprintf(stdout, "-P use the host bridge (bridge/, default localhost:18801) instead of tidal-hifi; -s then addresses the bridge\n");
    fprintf(stdout, "-T access token of the host bridge (env MUSIC_BRIDGE_TOKEN; saved setting otherwise)\n");
    fprintf(stdout, "-S port: be the hub on this port (e.g. 18322): sources such as the PC bridge connect to it and pair with a code shown in Settings > Sources\n");
    fprintf(stdout, "-F fake playlists/queue behind the list button (replaces the bridge's browse data)\n");
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
        host_from_cli = true;
    }

    const char * env_w = getenv("LV_SIM_WINDOW_WIDTH");
    const char * env_h = getenv("LV_SIM_WINDOW_HEIGHT");
    /* Default values */
    settings.window_width = atoi(env_w ? env_w : "800");
    settings.window_height = atoi(env_h ? env_h : "480");

    /* Parse the command-line options. */
    while((opt = getopt(argc, argv, "b:fmW:H:R:s:T:S:BVDFPh")) != -1) {
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
                host_from_cli = true;
                cli_server    = optarg;
                break;
            case 'T':
                cli_token = optarg;
                break;
            case 'S':
                hub_port = atoi(optarg);
                if(hub_port < 1024 || hub_port > 65535) die("Invalid -S port '%s'\n", optarg);
                break;
            case 'D':
                show_demo_screen = true;
                break;
            case 'F':
                fake_browse = true;
                break;
            case 'P':
                use_bridge = true;
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
    /* A long press on the cover opens the settings: make it deliberate (LVGL's default is 400 ms) */
    for(lv_indev_t * i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
        if(lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) lv_indev_set_long_press_time(i, 800);

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
        MusicAppDeps deps;
        deps.store = settings_store_create();
        AppSettings saved;
        std::unique_ptr<MediaSource> source;
        if(hub_port > 0) {
            deps.store->load(saved);
            std::random_device rd;
            auto hex = [&](int bytes) {
                static const char H[] = "0123456789abcdef";
                std::string s;
                for(int i = 0; i < bytes; i++) { unsigned v = rd() & 0xFF; s += H[v >> 4]; s += H[v & 15]; }
                return s;
            };
            if(saved.hub_device_id.empty()) saved.hub_device_id = hex(8);
            if(saved.hub_device_name.empty()) saved.hub_device_name = "Music Controller (simulator)";
            deps.store->save(saved); /* keeps the generated identity */
            std::vector<PairedSource> paired;
            nlohmann::json list = nlohmann::json::parse(saved.hub_sources.empty() ? "[]" : saved.hub_sources, nullptr, false);
            if(list.is_array())
                for(const auto & e : list)
                    if(e.is_object() && e.value("id", "") != "")
                        paired.push_back({e.value("id", ""), e.value("name", ""), e.value("secret", "")});
            auto hub = std::make_unique<HubSource>(ws_server_create(), HubIdentity{saved.hub_device_id, saved.hub_device_name}, paired);
            if(!hub->start((uint16_t)hub_port)) die("Cannot listen on port %d (is another hub or the bridge using it?)\n", hub_port);
            /* sources find the hub by itself: _musicbridge._tcp with the device id and name in the TXT record */
            g_mdns = mdns_advertiser_create();
            MdnsService adv;
            adv.instance = saved.hub_device_name;
            adv.port     = (uint16_t)hub_port;
            adv.txt      = {{"proto", "1"}, {"id", saved.hub_device_id}, {"name", saved.hub_device_name}};
            if(!g_mdns->start(adv)) fprintf(stderr, "mDNS: no usable network interface, the hub is not announced (sources need its address)\n");
            deps.hub = hub.get();
            source   = std::move(hub);
        } else if(use_bridge) {
            /* -s or the default only: the saved server address is tidal-hifi's (the settings page would
             * overwrite it while testing the bridge) */
            deps.store->load(saved);
            if(cli_token.empty()) {
                if(const char * env_token = getenv("MUSIC_BRIDGE_TOKEN")) cli_token = env_token;
            }
            if(!cli_token.empty()) saved.bridge_token = cli_token; /* what actually runs, like the server address */
            source = std::make_unique<BridgeSource>(cli_server, saved.bridge_token, saved.player);
        } else {
            if(deps.store->load(saved) && !host_from_cli && !saved.server.empty()) tidal_cfg.parse(saved.server);
            source = std::make_unique<TidalClient>(tidal_cfg);
        }
        saved.server          = source->endpoint(); /* what actually runs, whichever way it was chosen */
        if(fake_browse) deps.browse = fake_browse_create();
        else if(use_bridge) deps.browse = std::make_unique<BridgeBrowse>(cli_server, saved.bridge_token);
        deps.settings         = saved;
        deps.source           = std::move(source);
        deps.wifi             = wifi_manager_create(); /* simulator: fake networks */
        deps.validate_server  = [](const std::string & spec) {
            TidalConfig c;
            return c.parse(spec);
        };
        music_app_start(std::move(deps));
        install_volume_input();
    }

    signal(SIGINT, on_quit_signal);
    signal(SIGTERM, on_quit_signal);
    while(!g_quit) {
        uint32_t ms = lv_timer_handler();
        if(ms == LV_NO_TIMER_READY) {
            ms = LV_DEF_REFR_PERIOD;
        }
        usleep(ms * 1000);
    }

    if(g_mdns) g_mdns->stop(); /* goodbye: sources drop us from their lists at once instead of after the TTL */
    _exit(0); /* the background threads of the poller and the hub never stop; skip static destructors */
}
