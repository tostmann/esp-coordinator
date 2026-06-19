#pragma once
//
// netcfg — WiFi STA credential storage for the wifi-coex variant.
//
// Holds the single boot-time decision input for the app::start_int mode gate
// (see app.cpp): if STA creds are present we boot into Mode B (WiFi + NCP-over-
// TCP); if absent we boot into Mode A (Improv-Serial provisioning over the
// USB-Serial/JTAG line). Creds live in a dedicated NVS namespace ("netcfg") in
// the existing `nvs` partition — no partition-table change. The Improv
// onConnected callback calls netcfg_persist(); a host/factory-reset path calls
// netcfg_clear() to drop back to Mode A.
//
// nvs_flash_init() is already done in app::init() before any of these run.
//
#include <esp_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Max sizes per the WiFi spec (incl. NUL terminator headroom the callers size
// their buffers to). SSID <= 32 chars, WPA2 PSK <= 63 chars.
#define NETCFG_SSID_MAXLEN 33
#define NETCFG_PSK_MAXLEN  64

// true if a non-empty SSID is stored — the Mode B gate condition.
bool netcfg_has_sta_creds(void);

// Load stored creds into caller buffers. ssid_len >= NETCFG_SSID_MAXLEN,
// psk_len >= NETCFG_PSK_MAXLEN. Returns ESP_OK only if a non-empty ssid was
// read; psk may be empty (open network). On any error the buffers are cleared.
esp_err_t netcfg_load(char *ssid, size_t ssid_len, char *psk, size_t psk_len);

// Persist creds (atomic commit). psk may be NULL/empty for an open network.
esp_err_t netcfg_persist(const char *ssid, const char *psk);

// Erase stored creds — next boot enters Mode A (provisioning).
esp_err_t netcfg_clear(void);

#ifdef __cplusplus
}
#endif
