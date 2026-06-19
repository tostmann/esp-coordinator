#pragma once

#include <cstdint>
#include <cstddef>

namespace improv_wifi_busware {

// Generic IMPROV-Serial magic-filter for transports that share a single
// serial line between the Improv-Serial protocol and a user-facing console
// (CLI / REPL / vendor binary protocol).
//
// Why this exists
// ---------------
// Without a pre-filter, both the Improv parser and the user-facing console
// see the same byte stream. As soon as the Improv host sends `IMPROV<...>\n`,
// the console — being line-oriented — typically replies with something like
// `? (IMPROV<garbage> is unknown)\n`. That reply text *contains* the literal
// `IMPROV` magic, which is enough to make any strict Improv parser on the
// host (ESP Web Tools, `tools/improv_client.py`) misinterpret the noise as
// a malformed Improv frame. Subsequent valid responses are then dropped
// because the host is mid-discard waiting for "the rest of the bogus frame".
//
// What it does
// ------------
// SerialFilter is a tiny state machine that classifies each received byte as
// either part of an IMPROV frame (magic + 3 header bytes + N body bytes +
// 1 checksum) or as console traffic. It then forwards each byte to the
// matching sink:
//
//   * Console sink     -> bytes outside an IMPROV frame
//   * Improv  sink     -> bytes belonging to a fully-matched IMPROV frame
//
// The filter holds back lone partial-magic bytes (e.g. a stray "I" the user
// just typed) and flushes them to the console sink atomically when the next
// byte breaks the magic match — so a typed "I<enter>" still reaches the
// console as it would without the filter.
//
// Threading
// ---------
// Stateful, **not** thread-safe. Feed bytes from one task only. The typical
// use-site is the transport's RX-task; Improv tick() lives in another task
// reading from a stream-buffer the Improv sink writes into. As long as RX
// is single-tasked, no locking is required.
//
// Lifecycle
// ---------
// Allocate one instance per transport. Call setSinks() once before the first
// feed(). When the Improv window closes (ImprovWiFi::isArmed() goes false)
// you typically detach the filter from the transport entirely; reset() is
// available if you want to keep the same instance around for re-arming in
// some future variant.
class SerialFilter {
public:
    // Sink for one or more contiguous bytes. The opaque user pointer is
    // returned to the application unchanged so the same C function pointer
    // can demultiplex to multiple transports / instances.
    using SinkFn = void (*)(const uint8_t* data, size_t len, void* user);

    // Wire up the two sinks. Either may be nullptr; in that case the filter
    // silently drops bytes destined to that sink. Practical use is leaving
    // the Improv sink null while the window is closed, so no allocation
    // occurs — but typically you'd just stop calling feed() in that case.
    void setSinks(SinkFn toConsole, void* consoleUser,
                  SinkFn toImprov,  void* improvUser);

    // Reset the state machine. Any held-back partial-magic bytes are dropped.
    // Use when re-attaching the filter to a fresh stream (rare).
    void reset();

    // Feed one or more received bytes. Sinks are invoked synchronously and
    // may be called multiple times for a single feed() (e.g. when a body
    // and a non-magic byte alternate). Bytes are dispatched in stream order.
    void feed(const uint8_t* data, size_t len);

private:
    enum class Phase : uint8_t {
        Idle,    // outside any frame, scanning for 'I'
        Magic,   // matched 'I'..'V' progressively
        Header,  // got full magic, collecting ver/type/len bytes
        Body,    // counting down payload + checksum
    };

    static constexpr uint8_t kMagic[6] = {'I','M','P','R','O','V'};

    Phase    phase_     = Phase::Idle;
    uint8_t  hold_[8]   = {};   // up to magic(6) or header(3); never both
    uint8_t  holdN_     = 0;
    uint16_t bodyLeft_  = 0;    // payload-len + 1 (checksum)
    SinkFn   toConsole_ = nullptr;
    SinkFn   toImprov_  = nullptr;
    void*    consoleU_  = nullptr;
    void*    improvU_   = nullptr;
};

}  // namespace improv_wifi_busware
