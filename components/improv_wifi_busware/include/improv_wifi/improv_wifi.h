#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "improv_wifi/types.h"
#include "improv_wifi/wifi_backend.h"

namespace improv_wifi_busware {

// Sink for outgoing serial bytes. The library never owns the transport;
// the application provides this callback so the same code can write to
// an Arduino Stream, ESP-IDF usb_serial_jtag, a UART driver, or anything else.
using WriteFn = void (*)(const uint8_t* data, size_t len, void* user);

// Optional callbacks. Each receives the user pointer from Config::userCtx.
using OnErrorFn   = void (*)(Error err, void* user);
using OnConnectFn = void (*)(const char* ssid, const char* password, void* user);

struct DeviceInfo {
    ChipFamily  chipFamily       = ChipFamily::Esp32;
    const char* firmwareName     = "Unknown";
    const char* firmwareVersion  = "0.0.0";
    const char* deviceName       = "Device";
    const char* deviceUrl        = nullptr;  // nullptr -> http://<ip>; supports "{LOCAL_IPV4}"
};

struct Config {
    WiFiBackend* backend       = nullptr;     // required
    WriteFn      write         = nullptr;     // required
    void*        userCtx       = nullptr;     // passed back to all callbacks + write
    uint32_t     windowMs      = 120'000;     // bounded provisioning window after boot
    DeviceInfo   device{};                    // populate before first byte arrives
    OnErrorFn    onError       = nullptr;
    OnConnectFn  onConnected   = nullptr;
};

// Improv-Serial WiFi provisioning core. See README for end-to-end semantics.
//
// Lifecycle contract (see project memory `project_lifecycle.md`):
//   1. ctor: window opens; isArmed() == true.
//   2. After Config::windowMs of wall-clock has elapsed, the window closes
//      permanently for this boot cycle: isArmed() returns false, feedByte()
//      and tick() become no-ops, and no frames are written to the transport.
//   3. The window is unconditional — it opens regardless of provisioning
//      state so a connected device can still be re-provisioned within the
//      window after a reboot. Successful WIFI_SETTINGS does NOT shorten it
//      (the Web Flasher continues querying GET_DEVICE_INFO / state after
//      successful provisioning).
class ImprovWiFi {
public:
    explicit ImprovWiFi(const Config& cfg);

    // Application feeds bytes received on the serial transport into here.
    // Should be called only while isArmed() returns true; if called after
    // expiry the call is a cheap no-op and produces no output.
    void feedByte(uint8_t b);
    void feedBytes(const uint8_t* data, size_t len);

    // Drive the timeout and scan-completion polling. Pass current monotonic
    // time in milliseconds (e.g. millis() / esp_timer_get_time()/1000).
    void tick(uint32_t nowMs);

    // Window state. Once false, stays false until the device reboots.
    bool isArmed() const { return armed_; }

    // Remaining ms in the provisioning window relative to the supplied
    // monotonic clock (same time base used to feed tick()). Useful for
    // status-LED blink rates, frontend countdown displays, or log lines
    // like "Improv idle: 47s remaining".
    //
    // Contract:
    //   - returns 0 once the window has closed (isArmed() == false);
    //   - returns the full configured windowMs while armed but tick() has
    //     not yet been called (clock not yet anchored);
    //   - otherwise returns the live countdown clamped at 0.
    uint32_t windowMsRemaining(uint32_t nowMs) const {
        if (!armed_) return 0;
        if (!clockAnchored_) return cfg_.windowMs;
        const int32_t delta = static_cast<int32_t>(deadlineMs_ - nowMs);
        return delta > 0 ? static_cast<uint32_t>(delta) : 0;
    }

    // Convenience proxy to the WiFi backend.
    bool isConnected();

    // Replace device info (string lifetimes are caller-owned; use literals
    // or storage that outlives the ImprovWiFi instance).
    void setDeviceInfo(const DeviceInfo& info);

private:
    static constexpr size_t kBufferSize = 300;          // 9 + 255 + 1 = 265, rounded up
    static constexpr uint8_t kHeader[6] = {'I','M','P','R','O','V'};

    void parseByte_(uint8_t b);
    bool acceptByteAt_(size_t pos, uint8_t b) const;
    void onCompletePacket_();
    bool dispatchCommand_(const ImprovCommand& cmd);
    void handleWifiSettings_(const std::string& ssid, const std::string& pw);
    void handleGetWifiNetworks_();
    void pumpScan_();

    void emitState_(State s);
    void emitError_(Error e);
    void emitRpcResponse_(Command cmd, const std::vector<std::string>& fields);
    void emitDeviceUrl_(Command cmd);
    void writeFrame_(FrameType type, const uint8_t* payload, size_t plen);

    static ImprovCommand parsePayload_(const uint8_t* data, size_t length, bool withChecksum);

    Config       cfg_;
    DeviceInfo   dev_;
    uint8_t      buf_[kBufferSize];
    uint16_t     pos_         = 0;
    bool         armed_       = true;
    bool         clockAnchored_ = false;
    uint32_t     deadlineMs_  = 0;

    enum class ScanPhase { Idle, Awaiting, Done } scanPhase_ = ScanPhase::Idle;
};

}  // namespace improv_wifi_busware
