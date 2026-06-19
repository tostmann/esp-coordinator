#pragma once

// Legacy-compat header for the Arduino consumer surface that jnthas's
// upstream and ip4knx's main.cpp use:
//
//   #include "ImprovWiFiLibrary.h"
//   ImprovWiFi improvSerial(&Serial);
//   improvSerial.setDeviceInfo(...);
//   improvSerial.onImprovConnected(...);
//   improvSerial.onImprovError(...);
//   improvSerial.handleSerial();
//
// New code (CULFW32, future Busware projects) should bypass this header and
// use the transport-neutral core directly via <improv_wifi/improv_wifi.h>.
//
// Only effective when building under the Arduino framework. On pure ESP-IDF
// (CULFW32) including this header is a no-op so accidental inclusion does
// not pull Arduino-Core symbols into the build.

#include "ImprovTypes.h"
#include "improv_wifi/improv_wifi.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <Stream.h>

#include "improv_wifi/arduino_backend.h"

// Default window length for the legacy IMPROV_RUN_FOR macro. The lib's
// internal default is 120 s; this macro can still override it for backward
// compatibility with vendored ip4knx code that set IMPROV_RUN_FOR explicitly.
#ifndef IMPROV_RUN_FOR
  #define IMPROV_RUN_FOR 120000
#endif

class ImprovWiFi {
public:
    typedef void(OnImprovError)(ImprovTypes::Error);
    typedef void(OnImprovConnected)(const char* ssid, const char* password);
    typedef bool(CustomConnectWiFi)(const char* ssid, const char* password);

    explicit ImprovWiFi(Stream* serial)
        : serial_(serial), backend_(),
          core_(makeConfig_(this)) {}

    // --- Original public API ------------------------------------------------
    void handleSerial();
    bool handleBuffer(uint8_t* buffer, uint16_t bytes);

    void setDeviceInfo(ImprovTypes::ChipFamily chipFamily,
                       const char* firmwareName,
                       const char* firmwareVersion,
                       const char* deviceName);
    void setDeviceInfo(ImprovTypes::ChipFamily chipFamily,
                       const char* firmwareName,
                       const char* firmwareVersion,
                       const char* deviceName,
                       const char* deviceUrl);

    void onImprovError(OnImprovError* cb)         { onErrorCb_     = cb; }
    void onImprovConnected(OnImprovConnected* cb) { onConnectedCb_ = cb; }
    void setCustomConnectWiFi(CustomConnectWiFi* cb) { customConnectCb_ = cb; }

    bool tryConnectToWifi(const char* ssid, const char* password);
    bool isConnected();

private:
    static improv_wifi_busware::Config makeConfig_(ImprovWiFi* self);
    static void  writeTrampoline_(const uint8_t* d, size_t n, void* user);
    static void  errorTrampoline_(improv_wifi_busware::Error e, void* user);
    static void  connectTrampoline_(const char* s, const char* p, void* user);

    // Custom connect indirection -- if the user installed a CustomConnectWiFi
    // we have to wrap the WiFiBackend. Simplest way: keep using the default
    // ArduinoWiFiBackend, and short-circuit tryConnect() inside this facade
    // when customConnectCb_ is set.
    class WrappedBackend : public improv_wifi_busware::WiFiBackend {
    public:
        WrappedBackend() = default;
        void bind(ImprovWiFi* parent) { parent_ = parent; }
        bool isConnected() override            { return inner_.isConnected(); }
        std::string currentIp() override       { return inner_.currentIp(); }
        bool tryConnect(const char* s, const char* p) override {
            if (parent_ && parent_->customConnectCb_) return parent_->customConnectCb_(s, p);
            return inner_.tryConnect(s, p);
        }
        void startScan() override              { inner_.startScan(); }
        int  scanResult() override             { return inner_.scanResult(); }
        improv_wifi_busware::ApRecord apRecord(int i) override { return inner_.apRecord(i); }
        void clearScan() override              { inner_.clearScan(); }
    private:
        improv_wifi_busware::ArduinoWiFiBackend inner_{};
        ImprovWiFi* parent_ = nullptr;
    };

    Stream*                              serial_           = nullptr;
    WrappedBackend                       backend_{};
    improv_wifi_busware::ImprovWiFi      core_;

    OnImprovError*       onErrorCb_       = nullptr;
    OnImprovConnected*   onConnectedCb_   = nullptr;
    CustomConnectWiFi*   customConnectCb_ = nullptr;

    // Storage for device-info strings so ChipFamily/string lifetime is owned
    // by the facade, not the caller's stack.
    std::string firmwareName_;
    std::string firmwareVersion_;
    std::string deviceName_;
    std::string deviceUrl_;
};

#endif  // ARDUINO
