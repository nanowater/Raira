#pragma once

#include <memory>
#include <string>
#include <vector>

/* WiFi control for the settings page. Everything is non-blocking: the UI starts an operation and polls
 * its state from an LVGL timer. Simulator: a mock with fake networks (src/platform_sim/wifi_mock.cpp);
 * ESP32: esp_wifi (STA mode) driven by its event loop. */
struct WifiNetwork
{
    std::string ssid;
    int rssi     = -100; /* dBm, closer to 0 is stronger */
    bool secured = true;
};

enum class WifiState { Idle, Connecting, Connected, Failed };

class WifiManager {
  public:
    virtual ~WifiManager() = default;

    /* Start a scan. Results are available through scan_result() once scan_running() is false. */
    virtual void start_scan()                            = 0;
    virtual bool scan_running() const                    = 0;
    virtual std::vector<WifiNetwork> scan_result() const = 0; /* strongest first */

    /* Start connecting (empty password for open networks). Poll state(). */
    virtual void connect(const std::string & ssid, const std::string & password) = 0;
    virtual WifiState state() const                                              = 0;
    virtual std::string connected_ssid() const                                   = 0; /* "" unless Connected */
};

/* Provided by the platform layer. */
std::unique_ptr<WifiManager> wifi_manager_create();
