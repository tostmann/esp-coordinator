#pragma once
// Issue #17 diagnostics: USB-Serial/JTAG controller events and a periodic status
// line on the UART0 console. Everything here compiles to nothing unless
// CONFIG_NCP_DEBUG_USJ_STATUS is set, so the hooks in transport/app/main cost
// nothing in release builds.
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
#else
inline void start() {}
inline void count_usb_rx(int) {}
inline void count_usb_write(bool) {}
inline void count_route(uint8_t) {}
inline void count_app_loop() {}
inline void count_app_event(int) {}
#endif
}  // namespace diag
