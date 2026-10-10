#pragma once
// Issue #17 diagnostics: USB-Serial/JTAG controller events, a periodic status
// line on the UART0 console and an event log in RAM that the host can read
// over the NCP link (DEBUG_USJ_LOG). Everything here compiles to nothing
// unless CONFIG_NCP_DEBUG_USJ_STATUS is set, so the hooks in
// transport/app/main cost nothing in release builds.
#include <cstddef>
#include <cstdint>
#include "sdkconfig.h"

namespace diag {
#if CONFIG_NCP_DEBUG_USJ_STATUS
void start();                       // app_main, right after boot_guard::init()
void count_usb_rx(int bytes);       // transport USB RX task, per read
void count_usb_write(bool ok);      // transport write_int, per USB write attempt
void count_route(uint8_t iface);    // transport route flip (1 = USB, 2 = UART)
void count_app_loop();              // app event pump, per loop iteration
void count_app_event(int event);    // app event pump, per processed event
// DEBUG_USJ_LOG: copies up to `len` bytes of the log image at `offset` to
// `out` and returns the count; `*total` receives the image size. offset 0
// takes a fresh snapshot, so one pass from 0 to total is consistent.
size_t log_read(uint32_t offset, uint8_t* out, size_t len, uint32_t* total);
#else
inline void start() {}
inline void count_usb_rx(int) {}
inline void count_usb_write(bool) {}
inline void count_route(uint8_t) {}
inline void count_app_loop() {}
inline void count_app_event(int) {}
#endif
}  // namespace diag
