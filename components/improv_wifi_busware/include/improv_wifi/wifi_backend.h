#pragma once

#include <cstdint>
#include <string>

namespace improv_wifi_busware {

struct ApRecord {
    std::string ssid;
    int32_t     rssi      = 0;
    bool        needsAuth = false;
};

// Abstracts the WiFi side of provisioning so the Improv core stays
// transport-neutral. Two implementations ship with this library:
//
//   - ArduinoWiFiBackend (src/backend_arduino.cpp, when ARDUINO defined)
//   - EspIdfWiFiBackend  (src/backend_idf.cpp,     when ESP-IDF without Arduino)
//
// Applications can also provide their own implementation if they manage
// WiFi differently (e.g. captive portal, BLE provisioning intermediary, ...).
class WiFiBackend {
public:
    virtual ~WiFiBackend() = default;

    // Connection-state introspection.
    virtual bool        isConnected() = 0;
    virtual std::string currentIp()   = 0;  // "1.2.3.4" or "" when not connected.

    // Synchronous connect. Blocking up to a handful of seconds is acceptable
    // here; the Improv-Serial flow waits for a result before continuing.
    virtual bool tryConnect(const char* ssid, const char* password) = 0;

    // Non-blocking scan: kick off once, poll until scanResult() returns >= 0.
    static constexpr int kScanRunning = -1;
    static constexpr int kScanFailed  = -2;

    virtual void     startScan() = 0;
    virtual int      scanResult() = 0;       // kScanRunning / kScanFailed / >= 0
    virtual ApRecord apRecord(int index) = 0;
    virtual void     clearScan() = 0;
};

}  // namespace improv_wifi_busware
