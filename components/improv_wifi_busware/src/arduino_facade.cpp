#ifdef ARDUINO

#include "ImprovWiFiLibrary.h"

#include <Arduino.h>

namespace ipw = improv_wifi_busware;

ipw::Config ImprovWiFi::makeConfig_(ImprovWiFi* self) {
    self->backend_.bind(self);
    ipw::Config c;
    c.backend     = &self->backend_;
    c.write       = &ImprovWiFi::writeTrampoline_;
    c.userCtx     = self;
    c.windowMs    = IMPROV_RUN_FOR;
    c.onError     = &ImprovWiFi::errorTrampoline_;
    c.onConnected = &ImprovWiFi::connectTrampoline_;
    return c;
}

void ImprovWiFi::writeTrampoline_(const uint8_t* d, size_t n, void* user) {
    auto* self = static_cast<ImprovWiFi*>(user);
    if (self && self->serial_) self->serial_->write(d, n);
}

void ImprovWiFi::errorTrampoline_(ipw::Error e, void* user) {
    auto* self = static_cast<ImprovWiFi*>(user);
    if (self && self->onErrorCb_) {
        self->onErrorCb_(static_cast<ImprovTypes::Error>(e));
    }
}

void ImprovWiFi::connectTrampoline_(const char* s, const char* p, void* user) {
    auto* self = static_cast<ImprovWiFi*>(user);
    if (self && self->onConnectedCb_) self->onConnectedCb_(s, p);
}

void ImprovWiFi::handleSerial() {
    core_.tick(millis());
    if (!core_.isArmed() || !serial_) return;
    while (serial_->available() > 0) {
        const int b = serial_->read();
        if (b < 0) break;
        core_.feedByte(static_cast<uint8_t>(b));
    }
}

bool ImprovWiFi::handleBuffer(uint8_t* buffer, uint16_t bytes) {
    core_.tick(millis());
    if (!core_.isArmed() || !buffer) return false;
    core_.feedBytes(buffer, bytes);
    return true;
}

void ImprovWiFi::setDeviceInfo(ImprovTypes::ChipFamily chipFamily,
                               const char* firmwareName,
                               const char* firmwareVersion,
                               const char* deviceName) {
    firmwareName_    = firmwareName    ? firmwareName    : "";
    firmwareVersion_ = firmwareVersion ? firmwareVersion : "";
    deviceName_      = deviceName      ? deviceName      : "";
    ipw::DeviceInfo info;
    info.chipFamily      = static_cast<ipw::ChipFamily>(chipFamily);
    info.firmwareName    = firmwareName_.c_str();
    info.firmwareVersion = firmwareVersion_.c_str();
    info.deviceName      = deviceName_.c_str();
    info.deviceUrl       = deviceUrl_.empty() ? nullptr : deviceUrl_.c_str();
    core_.setDeviceInfo(info);
}

void ImprovWiFi::setDeviceInfo(ImprovTypes::ChipFamily chipFamily,
                               const char* firmwareName,
                               const char* firmwareVersion,
                               const char* deviceName,
                               const char* deviceUrl) {
    deviceUrl_ = deviceUrl ? deviceUrl : "";
    setDeviceInfo(chipFamily, firmwareName, firmwareVersion, deviceName);
}

bool ImprovWiFi::tryConnectToWifi(const char* ssid, const char* password) {
    if (customConnectCb_) return customConnectCb_(ssid, password);
    return backend_.tryConnect(ssid, password);
}

bool ImprovWiFi::isConnected() { return backend_.isConnected(); }

#endif  // ARDUINO
