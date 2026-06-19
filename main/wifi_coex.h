#pragma once
//
// wifi_coex — WiFi STA bring-up + WiFi/802.15.4 radio coexistence for the
// wifi-coex Mode B (NCP-over-TCP). On the ESP32-C6 a single 2.4 GHz radio is
// time-division-shared between WiFi and the native 802.15.4 (Zigbee) MAC via
// the software coexistence arbiter; this is the experimental "C1" (Espressif:
// supported-but-unstable) coordinator-with-WiFi case. See memory wifi-coex-branch.
//
// Lifecycle (mirrors the IDF esp_zigbee_gateway SKIP_STARTUP flow, which was
// HW-confirmed on a C6 in the coex smoke test):
//   1. wifi_coex_init()           — app::start_int (Mode B), BEFORE the Zigbee
//                                    stack starts: netif + default event loop +
//                                    esp_wifi_init(STA) + WIFI_PS_NONE (C6 rule)
//                                    + set_config from netcfg creds. No connect.
//   2. wifi_coex_enable_radio_coex() — from zb_ncp::continue_zboss
//                                    (ZB_ZDO_SIGNAL_SKIP_STARTUP), guarded by
//                                    CONFIG_ESP_COEX_SW_COEXIST_ENABLE. Same
//                                    call site the gateway uses.
//   3. wifi_coex_start_connect()  — right after, esp_wifi_start + non-blocking
//                                    esp_wifi_connect(). got-IP arrives async.
//
// All entry points no-op if wifi_coex_init() has not run (i.e. Mode A / no creds).
//
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_coex_init(void);
void      wifi_coex_enable_radio_coex(void);
esp_err_t wifi_coex_start_connect(void);
bool      wifi_coex_is_up(void);          // true once IP_EVENT_STA_GOT_IP fired
void      wifi_coex_current_ip(char *buf, size_t len);  // "1.2.3.4", or "" when no IP yet

#ifdef __cplusplus
}
#endif
