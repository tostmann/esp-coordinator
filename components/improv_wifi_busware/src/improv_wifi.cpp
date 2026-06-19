#include "improv_wifi/improv_wifi.h"

#include <cstring>

namespace improv_wifi_busware {

constexpr uint8_t ImprovWiFi::kHeader[6];

ImprovWiFi::ImprovWiFi(const Config& cfg)
    : cfg_(cfg), dev_(cfg.device) {}

void ImprovWiFi::setDeviceInfo(const DeviceInfo& info) {
    dev_ = info;
}

bool ImprovWiFi::isConnected() {
    return cfg_.backend && cfg_.backend->isConnected();
}

void ImprovWiFi::feedByte(uint8_t b) {
    if (!armed_) return;
    parseByte_(b);
}

void ImprovWiFi::feedBytes(const uint8_t* data, size_t len) {
    if (!armed_ || !data) return;
    for (size_t i = 0; i < len; ++i) parseByte_(data[i]);
}

void ImprovWiFi::tick(uint32_t nowMs) {
    if (!armed_) return;

    if (!clockAnchored_) {
        clockAnchored_ = true;
        deadlineMs_    = nowMs + cfg_.windowMs;
    }

    // 32-bit-safe expiry check (handles wraparound of millis()).
    if (static_cast<int32_t>(nowMs - deadlineMs_) >= 0) {
        armed_ = false;
        pos_   = 0;
        if (scanPhase_ != ScanPhase::Idle && cfg_.backend) cfg_.backend->clearScan();
        scanPhase_ = ScanPhase::Idle;
        return;
    }

    pumpScan_();
}

void ImprovWiFi::parseByte_(uint8_t b) {
    if (pos_ >= kBufferSize) {
        pos_ = 0;
        return;
    }
    if (acceptByteAt_(pos_, b)) {
        buf_[pos_++] = b;
        // Once we know the full length, see if this byte was the checksum.
        if (pos_ >= 9) {
            uint16_t total = 9 + buf_[8] + 1;  // header(6)+ver(1)+type(1)+len(1)+payload+checksum
            if (pos_ == total) {
                onCompletePacket_();
                pos_ = 0;
            }
        }
    } else {
        pos_ = 0;
    }
}

bool ImprovWiFi::acceptByteAt_(size_t pos, uint8_t b) const {
    if (pos < 6)  return b == kHeader[pos];
    if (pos == 6) return b == kImprovSerialVersion;
    // pos == 7 (type) and pos == 8 (length) accept any value; bounds-check
    // happens as the buffer is consumed.
    if (pos == 7 || pos == 8) return true;

    const uint16_t dataLen = buf_[8];
    const uint16_t end     = 9 + dataLen + 1;          // total wire length
    if (pos < 9 + dataLen) return true;                // payload byte
    if (pos == 9 + dataLen) {                          // this byte = checksum
        uint16_t sum = 0;
        for (size_t i = 0; i < pos; ++i) sum += buf_[i];
        return static_cast<uint8_t>(sum) == b;
    }
    // Should never get here because parseByte_() resets at total length.
    (void)end;
    return false;
}

void ImprovWiFi::onCompletePacket_() {
    const FrameType type   = static_cast<FrameType>(buf_[7]);
    const uint16_t  dlen   = buf_[8];
    const uint8_t*  payload = buf_ + 9;

    if (type != FrameType::Rpc) {
        // We do not act on inbound CurrentState/Error/RpcResponse frames.
        return;
    }

    // Parse RPC payload — note: the wire-level checksum has already been
    // verified by acceptByteAt_(), so the inner parser does not re-check.
    const ImprovCommand cmd = parsePayload_(payload, dlen, /*withChecksum=*/false);
    dispatchCommand_(cmd);
}

ImprovCommand ImprovWiFi::parsePayload_(const uint8_t* data, size_t length, bool withChecksum) {
    ImprovCommand out{Command::Unknown, "", ""};
    if (length < 2) return out;

    const Command cmd  = static_cast<Command>(data[0]);
    const uint8_t dlen = data[1];
    const size_t  fieldsStart = 2;
    const size_t  fieldsEnd   = length - (withChecksum ? 1 : 0);

    if (fieldsEnd < fieldsStart || dlen != fieldsEnd - fieldsStart) {
        return out;  // Command::Unknown -> caller emits ERROR_INVALID_RPC
    }

    if (withChecksum) {
        uint16_t sum = 0;
        for (size_t i = 0; i < length - 1; ++i) sum += data[i];
        if (static_cast<uint8_t>(sum) != data[length - 1]) {
            out.command = Command::BadChecksum;
            return out;
        }
    }

    if (cmd == Command::WifiSettings) {
        // [ssid_len][ssid][pass_len][pass]
        if (fieldsEnd - fieldsStart < 2) return out;
        const uint8_t ssidLen = data[2];
        if (3u + ssidLen + 1u > fieldsEnd) return out;
        const uint8_t passLen = data[3 + ssidLen];
        if (3u + ssidLen + 1u + passLen != fieldsEnd) return out;
        out.command  = cmd;
        out.ssid.assign(reinterpret_cast<const char*>(data + 3), ssidLen);
        out.password.assign(reinterpret_cast<const char*>(data + 3 + ssidLen + 1), passLen);
        return out;
    }

    out.command = cmd;
    return out;
}

bool ImprovWiFi::dispatchCommand_(const ImprovCommand& cmd) {
    if (cmd.command == Command::Unknown) {
        emitError_(Error::InvalidRpc);
        return false;
    }
    if (cmd.command == Command::BadChecksum) {
        emitError_(Error::InvalidRpc);
        return false;
    }

    switch (cmd.command) {
        case Command::GetCurrentState:
            if (isConnected()) {
                emitState_(State::Provisioned);
                emitDeviceUrl_(cmd.command);
            } else {
                emitState_(State::Authorized);
            }
            return true;

        case Command::WifiSettings:
            handleWifiSettings_(cmd.ssid, cmd.password);
            return true;

        case Command::GetDeviceInfo: {
            const char* family[] = {"ESP32", "ESP32-C3", "ESP32-S2", "ESP32-S3", "ESP32-C6", "ESP8266"};
            const auto idx = static_cast<size_t>(dev_.chipFamily);
            const char* fam = (idx < sizeof(family)/sizeof(family[0])) ? family[idx] : "ESP";
            emitRpcResponse_(cmd.command, {
                dev_.firmwareName     ? std::string(dev_.firmwareName)    : "",
                dev_.firmwareVersion  ? std::string(dev_.firmwareVersion) : "",
                std::string(fam),
                dev_.deviceName       ? std::string(dev_.deviceName)      : "",
            });
            return true;
        }

        case Command::GetWifiNetworks:
            handleGetWifiNetworks_();
            return true;

        case Command::Identify:
            // Optional cosmetic command; spec allows acknowledging with ERROR_NONE.
            emitError_(Error::None);
            return true;

        default:
            emitError_(Error::UnknownRpc);
            return false;
    }
}

void ImprovWiFi::handleWifiSettings_(const std::string& ssid, const std::string& pw) {
    if (ssid.empty() || ssid.length() > 32) {
        emitError_(Error::InvalidRpc);
        return;
    }
    if (pw.length() > 0 && pw.length() < 8) {
        emitError_(Error::InvalidRpc);
        return;
    }
    if (pw.length() > 63) {
        emitError_(Error::InvalidRpc);
        return;
    }

    emitState_(State::Provisioning);

    if (!cfg_.backend) {
        emitError_(Error::Unknown);
        return;
    }

    const bool ok = cfg_.backend->tryConnect(ssid.c_str(), pw.c_str());

    if (ok) {
        emitError_(Error::None);
        emitState_(State::Provisioned);
        emitDeviceUrl_(Command::WifiSettings);
        if (cfg_.onConnected) cfg_.onConnected(ssid.c_str(), pw.c_str(), cfg_.userCtx);
    } else {
        emitError_(Error::UnableToConnect);
        if (cfg_.onError) cfg_.onError(Error::UnableToConnect, cfg_.userCtx);
        // After failure return to STATE_AUTHORIZED so the user can try again
        // within the still-open window. Mirrors what ESP Web Tools expects.
        emitState_(State::Authorized);
    }
}

void ImprovWiFi::handleGetWifiNetworks_() {
    if (!cfg_.backend) {
        // No backend -> emit empty terminator.
        emitRpcResponse_(Command::GetWifiNetworks, {});
        return;
    }
    cfg_.backend->startScan();
    scanPhase_ = ScanPhase::Awaiting;
    // The application must drive tick() to poll completion; pumpScan_() will
    // emit per-AP frames + terminator once the backend reports a result.
}

void ImprovWiFi::pumpScan_() {
    if (scanPhase_ != ScanPhase::Awaiting || !cfg_.backend) return;

    const int n = cfg_.backend->scanResult();
    if (n == WiFiBackend::kScanRunning) return;

    if (n == WiFiBackend::kScanFailed || n <= 0) {
        // Single empty terminator -> ESP Web Tools displays "no networks".
        emitRpcResponse_(Command::GetWifiNetworks, {});
        cfg_.backend->clearScan();
        scanPhase_ = ScanPhase::Idle;
        return;
    }

    // Emit one RPC_RESPONSE per AP (Improv-Serial spec, ESP Web Tools relies
    // on this), de-duplicated by SSID; strongest RSSI wins.
    std::vector<ApRecord> aps;
    aps.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) aps.push_back(cfg_.backend->apRecord(i));

    // Sort by RSSI desc, then drop subsequent dups by SSID.
    std::vector<size_t> order(aps.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    for (size_t i = 0; i + 1 < order.size(); ++i) {
        for (size_t j = i + 1; j < order.size(); ++j) {
            if (aps[order[j]].rssi > aps[order[i]].rssi) {
                std::swap(order[i], order[j]);
            }
        }
    }

    for (size_t k = 0; k < order.size(); ++k) {
        const auto& ap = aps[order[k]];
        if (ap.ssid.empty()) continue;
        bool dup = false;
        for (size_t earlier = 0; earlier < k; ++earlier) {
            if (aps[order[earlier]].ssid == ap.ssid) { dup = true; break; }
        }
        if (dup) continue;
        emitRpcResponse_(Command::GetWifiNetworks, {
            ap.ssid,
            std::to_string(ap.rssi),
            ap.needsAuth ? "YES" : "NO",
        });
    }
    // Empty terminator marks end-of-scan.
    emitRpcResponse_(Command::GetWifiNetworks, {});

    cfg_.backend->clearScan();
    scanPhase_ = ScanPhase::Idle;
}

void ImprovWiFi::emitState_(State s) {
    const uint8_t payload[1] = { static_cast<uint8_t>(s) };
    writeFrame_(FrameType::CurrentState, payload, 1);
}

void ImprovWiFi::emitError_(Error e) {
    const uint8_t payload[1] = { static_cast<uint8_t>(e) };
    writeFrame_(FrameType::ErrorState, payload, 1);
}

void ImprovWiFi::emitRpcResponse_(Command cmd, const std::vector<std::string>& fields) {
    // Inner format: [cmd_id][total_len][len][bytes]...[len][bytes]
    std::vector<uint8_t> inner;
    inner.reserve(2 + 32);
    inner.push_back(static_cast<uint8_t>(cmd));
    inner.push_back(0);  // placeholder for total_len, filled below

    uint32_t totalLen = 0;
    for (const auto& s : fields) {
        if (s.size() > 255) {
            // Single field too large for a 1-byte length — bail rather than truncate.
            emitError_(Error::Unknown);
            return;
        }
        inner.push_back(static_cast<uint8_t>(s.size()));
        inner.insert(inner.end(), s.begin(), s.end());
        totalLen += 1 + s.size();
    }
    if (totalLen > 253) {
        // Inner payload + cmd_id + total_len byte cannot exceed 255 (frame length field).
        emitError_(Error::Unknown);
        return;
    }
    inner[1] = static_cast<uint8_t>(totalLen);
    writeFrame_(FrameType::RpcResponse, inner.data(), inner.size());
}

void ImprovWiFi::emitDeviceUrl_(Command cmd) {
    std::string ip = cfg_.backend ? cfg_.backend->currentIp() : "";
    std::string url;
    if (dev_.deviceUrl && *dev_.deviceUrl) {
        url = dev_.deviceUrl;
        const std::string token = "{LOCAL_IPV4}";
        size_t at = 0;
        while ((at = url.find(token, at)) != std::string::npos) {
            url.replace(at, token.size(), ip);
            at += ip.size();
        }
    } else {
        url = "http://" + ip;
    }
    emitRpcResponse_(cmd, { url });
}

void ImprovWiFi::writeFrame_(FrameType type, const uint8_t* payload, size_t plen) {
    if (!cfg_.write) return;
    if (plen > 255) return;  // length field is one byte

    // header(6) + ver(1) + type(1) + len(1) + payload + checksum(1)
    uint8_t frame[9 + 255 + 1];
    std::memcpy(frame, kHeader, 6);
    frame[6] = kImprovSerialVersion;
    frame[7] = static_cast<uint8_t>(type);
    frame[8] = static_cast<uint8_t>(plen);
    if (plen) std::memcpy(frame + 9, payload, plen);
    uint16_t sum = 0;
    for (size_t i = 0; i < 9 + plen; ++i) sum += frame[i];
    frame[9 + plen] = static_cast<uint8_t>(sum);

    // Sandwich with newlines so the ESP Web Tools serial parser, which
    // discards non-IMPROV bytes but uses '\n' as a sync point, resets
    // its line buffer between frames.
    const uint8_t nl = '\n';
    cfg_.write(&nl, 1, cfg_.userCtx);
    cfg_.write(frame, 9 + plen + 1, cfg_.userCtx);
    cfg_.write(&nl, 1, cfg_.userCtx);
}

}  // namespace improv_wifi_busware
