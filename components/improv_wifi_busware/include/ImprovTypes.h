#pragma once

// Legacy-compat header: keeps the original `ImprovTypes::*` enum surface that
// jnthas's library and ip4knx's main.cpp expect. New code should prefer
// `improv_wifi_busware::*` types from <improv_wifi/types.h>.

#include <cstdint>
#include <string>
#include <vector>

#include "improv_wifi/types.h"

#ifdef ARDUINO
  #include <Arduino.h>
#endif

#ifndef MAX_ATTEMPTS_WIFI_CONNECTION
  #define MAX_ATTEMPTS_WIFI_CONNECTION 20
#endif
#ifndef DELAY_MS_WAIT_WIFI_CONNECTION
  #define DELAY_MS_WAIT_WIFI_CONNECTION 500
#endif

namespace ImprovTypes {

enum Error : uint8_t {
    ERROR_NONE              = 0x00,
    ERROR_INVALID_RPC       = 0x01,
    ERROR_UNKNOWN_RPC       = 0x02,
    ERROR_UNABLE_TO_CONNECT = 0x03,
    ERROR_NOT_AUTHORIZED    = 0x04,
    ERROR_UNKNOWN           = 0xFF,
};

enum State : uint8_t {
    STATE_STOPPED               = 0x00,  // Internal use only
    STATE_AWAITING_AUTHORIZATION = 0x01,
    STATE_AUTHORIZED            = 0x02,
    STATE_PROVISIONING          = 0x03,
    STATE_PROVISIONED           = 0x04,
};

enum Command : uint8_t {
    UNKNOWN              = 0x00,
    WIFI_SETTINGS        = 0x01,
    GET_CURRENT_STATE    = 0x02,
    GET_DEVICE_INFO      = 0x03,
    GET_WIFI_NETWORKS    = 0x04,
    SET_HOSTNAME         = 0x05,
    SET_DEVICE_NAME      = 0x06,
    IDENTIFY             = 0x07,
    BAD_CHECKSUM         = 0xFF,
};

enum ImprovSerialType : uint8_t {
    TYPE_CURRENT_STATE = 0x01,
    TYPE_ERROR_STATE   = 0x02,
    TYPE_RPC           = 0x03,
    TYPE_RPC_RESPONSE  = 0x04,
};

enum ChipFamily : uint8_t {
    CF_ESP32      = 0,
    CF_ESP32_C3   = 1,
    CF_ESP32_S2   = 2,
    CF_ESP32_S3   = 3,
    CF_ESP32_C6   = 4,
    CF_ESP8266    = 5,
};

inline constexpr uint8_t IMPROV_SERIAL_VERSION = 0x01;
inline constexpr uint8_t CAPABILITY_IDENTIFY   = 0x01;

struct ImprovCommand {
    Command     command;
    std::string ssid;
    std::string password;
};

struct ImprovWiFiParamsStruct {
    std::string firmwareName;
    std::string firmwareVersion;
    ChipFamily  chipFamily;
    std::string deviceName;
    std::string deviceUrl;
};

}  // namespace ImprovTypes
