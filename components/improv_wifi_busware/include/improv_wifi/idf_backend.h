#pragma once

// ESP-IDF native WiFi backend for ImprovWiFi. Compiled only when ESP-IDF is
// the build framework AND we are not riding on top of arduino-esp32 (in which
// case ArduinoWiFiBackend is the right choice).
#if defined(ESP_PLATFORM) && !defined(ARDUINO)

#include <atomic>
#include <vector>

#include "improv_wifi/wifi_backend.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

namespace improv_wifi_busware {

// Default ESP-IDF WiFi backend. Uses esp_wifi_* directly; the application is
// responsible for having initialized NVS, the default event loop, and the
// netif subsystem (the typical ESP-IDF main() boilerplate). The backend is
// idempotent against repeated init -- if WiFi/netif is already up it keeps
// going, otherwise it brings them up itself.
class EspIdfWiFiBackend : public WiFiBackend {
public:
    struct Options {
        // Passed to esp_wifi_connect() retry loop.
        uint32_t connectTimeoutMs = 12'000;
        // Total scan listening time (ESP-IDF default ~120 ms per channel × N).
        // Leave 0 to use the IDF defaults.
        uint32_t maxScanTimeMs    = 0;
        // When multiple BSSIDs share the SSID we provision against, pick the
        // strongest one (WIFI_ALL_CHANNEL_SCAN + WIFI_CONNECT_AP_BY_SIGNAL).
        // Costs ~1-2 s of all-channel scan per connect attempt; cheap for a
        // one-shot provisioning step, painful as a hot path. Default on
        // because the typical provisioning environment has multi-AP coverage
        // and "first BSSID found" is rarely what the user wanted.
        bool     connectToStrongest = true;
    };

    EspIdfWiFiBackend();
    explicit EspIdfWiFiBackend(const Options& opts);
    ~EspIdfWiFiBackend() override;

    bool        isConnected() override;
    std::string currentIp() override;
    bool        tryConnect(const char* ssid, const char* password) override;

    void     startScan() override;
    int      scanResult() override;
    ApRecord apRecord(int index) override;
    void     clearScan() override;

private:
    static void wifiEvtTrampoline_(void* arg, esp_event_base_t base, int32_t id, void* data);
    static void ipEvtTrampoline_(void* arg, esp_event_base_t base, int32_t id, void* data);

    void onWifiEvent_(int32_t id, void* data);
    void onIpEvent_(int32_t id, void* data);
    void ensureBaseInit_();

    Options                          opts_;
    esp_netif_t*                     staNetif_   = nullptr;
    bool                             baseInit_   = false;
    bool                             eventsBound_ = false;

    std::atomic<bool>                connected_{false};
    std::atomic<bool>                connecting_{false};
    std::atomic<bool>                gotIp_{false};
    std::atomic<bool>                connectFailed_{false};

    std::atomic<bool>                scanRunning_{false};
    std::atomic<bool>                scanFailed_{false};
    std::atomic<int>                 scanCount_{0};
    std::vector<wifi_ap_record_t>    scanCache_;
};

}  // namespace improv_wifi_busware

#endif  // ESP_PLATFORM && !ARDUINO
