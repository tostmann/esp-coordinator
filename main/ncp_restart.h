#pragma once
// Reboot the device. Plain build: esp_restart(). ESPHome build: through
// ESPHome's own reboot path (ncp_host_restart() in the zboss_ncp component),
// so its shutdown hooks run and its debug component names the NCP as the
// reboot source instead of the previous one. Does not return.
#include "sdkconfig.h"
#include <esp_system.h>

#if CONFIG_NCP_ESPHOME
extern "C" [[noreturn]] void ncp_host_restart(void);
[[noreturn]] static inline void ncp_restart(void) { ncp_host_restart(); }
#else
[[noreturn]] static inline void ncp_restart(void) { esp_restart(); }
#endif
