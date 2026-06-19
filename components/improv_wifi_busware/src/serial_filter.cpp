#include "improv_wifi/serial_filter.h"

namespace improv_wifi_busware {

constexpr uint8_t SerialFilter::kMagic[6];

void SerialFilter::setSinks(SinkFn toConsole, void* consoleUser,
                            SinkFn toImprov,  void* improvUser) {
    toConsole_ = toConsole;
    consoleU_  = consoleUser;
    toImprov_  = toImprov;
    improvU_   = improvUser;
}

void SerialFilter::reset() {
    phase_    = Phase::Idle;
    holdN_    = 0;
    bodyLeft_ = 0;
}

void SerialFilter::feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        switch (phase_) {
        case Phase::Idle:
            if (b == kMagic[0]) {
                hold_[0] = b;
                holdN_   = 1;
                phase_   = Phase::Magic;
            } else if (toConsole_) {
                toConsole_(&b, 1, consoleU_);
            }
            break;

        case Phase::Magic:
            if (b == kMagic[holdN_]) {
                hold_[holdN_++] = b;
                if (holdN_ == 6) {
                    // Full magic — frame is committed to the Improv sink.
                    if (toImprov_) toImprov_(kMagic, 6, improvU_);
                    phase_ = Phase::Header;
                    holdN_ = 0;
                }
            } else {
                // Magic mismatch — flush the held-back partial-magic bytes
                // (e.g. user typed "I" then "<enter>") AND the current byte
                // back to the console as one block.
                if (toConsole_) toConsole_(hold_, holdN_, consoleU_);
                if (toConsole_) toConsole_(&b, 1, consoleU_);
                holdN_ = 0;
                phase_ = Phase::Idle;
            }
            break;

        case Phase::Header:
            // Header = ver(1) + type(1) + len(1). The lib parses these
            // itself; we just route them through and remember len so we
            // know how many body+checksum bytes still belong to the frame.
            if (toImprov_) toImprov_(&b, 1, improvU_);
            hold_[holdN_++] = b;
            if (holdN_ == 3) {
                bodyLeft_ = static_cast<uint16_t>(hold_[2]) + 1;  // payload + csum
                phase_    = (bodyLeft_ == 0) ? Phase::Idle : Phase::Body;
                holdN_    = 0;
            }
            break;

        case Phase::Body:
            if (toImprov_) toImprov_(&b, 1, improvU_);
            if (--bodyLeft_ == 0) phase_ = Phase::Idle;
            break;
        }
    }
}

}  // namespace improv_wifi_busware
