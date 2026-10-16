#include "platform/wifi.h"
#include <chrono>

/* Simulated WiFi so the settings page can be developed without hardware: a scan takes 1.2 s and
 * returns a few fixed networks; connecting takes 1.5 s and succeeds for open networks and for
 * passwords of 8+ characters (like WPA2), otherwise fails. It never touches the PC's real WiFi. */
namespace {

using Clock = std::chrono::steady_clock;

class MockWifi : public WifiManager {
public:
    void start_scan() override
    {
        scan_started_ = Clock::now();
        scanning_     = true;
    }

    bool scan_running() const override
    {
        if(scanning_ && Clock::now() - scan_started_ > std::chrono::milliseconds(1200)) scanning_ = false;
        return scanning_;
    }

    std::vector<WifiNetwork> scan_result() const override
    {
        return {{"HomeWiFi_5G", -42, true},      {"HomeWiFi", -55, true},
                {"cafe-guest", -63, false},      {"iptime-A1B2", -71, true},
                {"공유기_사무실", -78, true},    {"AndroidAP", -84, true}};
    }

    void connect(const std::string & ssid, const std::string & password) override
    {
        target_     = ssid;
        ok_         = password.empty() || password.size() >= 8;
        started_    = Clock::now();
        connecting_ = true;
        connected_.clear();
    }

    WifiState state() const override
    {
        if(connecting_ && Clock::now() - started_ > std::chrono::milliseconds(1500)) {
            connecting_ = false;
            if(ok_) connected_ = target_;
            else failed_ = true;
        }
        if(connecting_) return WifiState::Connecting;
        if(!connected_.empty()) return WifiState::Connected;
        return failed_ ? WifiState::Failed : WifiState::Idle;
    }

    std::string connected_ssid() const override { return state() == WifiState::Connected ? connected_ : ""; }

private:
    mutable bool scanning_   = false;
    mutable bool connecting_ = false;
    mutable bool failed_     = false;
    mutable std::string connected_;
    Clock::time_point scan_started_{};
    Clock::time_point started_{};
    std::string target_;
    bool ok_ = false;
};

} // namespace

std::unique_ptr<WifiManager> wifi_manager_create()
{
    return std::make_unique<MockWifi>();
}
