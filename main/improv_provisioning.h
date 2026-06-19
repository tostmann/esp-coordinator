#pragma once
//
// improv_provisioning — Mode A (no STA creds): Improv-Serial WiFi provisioning
// driven by the browser web-flasher (ESP Web Tools / improv-wifi) over the
// USB-Serial/JTAG line. No WebUI. The USB-JTAG endpoint carries Improv ONLY in
// this mode: the NCP boot frame and the Zigbee stack are NOT started (see
// app::start_int mode gate), so the DEAD frame stream can never collide with
// Improv frames (mode gate, not a per-byte demux).
//
// On a successful WifiSettings: persist creds to NVS (netcfg) and esp_restart()
// into Mode B (WiFi + NCP-over-TCP).
//
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t improv_provisioning_start(void);

// Mode B (wifi-coex, STA creds already present): a bounded 120 s post-boot
// Improv-Serial window on USB-Serial/JTAG so a user can re-enter WiFi creds
// without erasing/reflashing. Unlike Mode A it does NOT touch esp_wifi (that
// would fight the wifi_coex STA owner) — it captures the new creds and reboots;
// wifi_coex applies + validates them on the next boot. Requires USB-JTAG to be
// reserved for Improv first (transport::disable_usb_ncp()).
esp_err_t improv_provisioning_start_reconfig(void);

#ifdef __cplusplus
}
#endif
