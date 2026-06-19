#pragma once

#include <cstdint>
#include <string>

namespace improv_wifi_busware {

// Improv-Serial protocol error codes (improv-wifi.com/serial/).
enum class Error : uint8_t {
    None              = 0x00,
    InvalidRpc        = 0x01,
    UnknownRpc        = 0x02,
    UnableToConnect   = 0x03,
    NotAuthorized     = 0x04,
    Unknown           = 0xFF,
};

// Improv-Serial state values.
enum class State : uint8_t {
    Stopped               = 0x00,
    AwaitingAuthorization = 0x01,
    Authorized            = 0x02,
    Provisioning          = 0x03,
    Provisioned           = 0x04,
};

// RPC command IDs.
enum class Command : uint8_t {
    Unknown           = 0x00,
    WifiSettings      = 0x01,
    GetCurrentState   = 0x02,
    GetDeviceInfo     = 0x03,
    GetWifiNetworks   = 0x04,
    SetHostname       = 0x05,
    SetDeviceName     = 0x06,
    Identify          = 0x07,
    BadChecksum       = 0xFF,
};

enum class ChipFamily : uint8_t {
    Esp32     = 0,
    Esp32C3   = 1,
    Esp32S2   = 2,
    Esp32S3   = 3,
    Esp32C6   = 4,
    Esp8266   = 5,
};

// Frame type byte (offset 7 in the wire frame).
enum class FrameType : uint8_t {
    CurrentState = 0x01,
    ErrorState   = 0x02,
    Rpc          = 0x03,
    RpcResponse  = 0x04,
};

inline constexpr uint8_t kImprovSerialVersion = 0x01;
inline constexpr uint8_t kCapabilityIdentify  = 0x01;

struct ImprovCommand {
    Command     command;
    std::string ssid;
    std::string password;
};

}  // namespace improv_wifi_busware
