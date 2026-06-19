#pragma once

// Arduino-Core WiFi backend for ImprovWiFi. Compiled only when ARDUINO is
// defined (ip4knx and other arduino-esp32 / ESP8266 consumers).
#ifdef ARDUINO

#include "improv_wifi/wifi_backend.h"

namespace improv_wifi_busware {

// Arduino backend: wraps the WiFi class from <WiFi.h>/<ESP8266WiFi.h>.
// Uses the non-blocking scan flavour (WiFi.scanNetworks(true, false)) so the
// host application loop is not stalled during a GET_WIFI_NETWORKS call.
class ArduinoWiFiBackend : public WiFiBackend {
public:
    struct Options {
        uint32_t connectTimeoutMs = 12'000;
        uint32_t connectPollMs    = 250;
        // Pre-scan synchronously before WiFi.begin() and pass the strongest
        // matching BSSID + channel explicitly. Costs an extra ~3-5 s for the
        // synchronous scan but avoids the Arduino-Core driver picking the
        // first BSSID it happens to see when several share the SSID. Default
        // on for symmetry with EspIdfWiFiBackend; flip off when the provision
        // budget is tight.
        bool     connectToStrongest = true;
    };

    ArduinoWiFiBackend();
    explicit ArduinoWiFiBackend(const Options& opts);

    bool        isConnected() override;
    std::string currentIp() override;
    bool        tryConnect(const char* ssid, const char* password) override;

    void     startScan() override;
    int      scanResult() override;
    ApRecord apRecord(int index) override;
    void     clearScan() override;

private:
    Options opts_;
    bool    scanInFlight_ = false;
};

}  // namespace improv_wifi_busware

#endif  // ARDUINO
