// The firmware's wifi_coex_* entry points for the ESPHome build: ESPHome owns
// the network (WiFi or Ethernet), so these ask it through the hooks the
// component sets (ncp_host_set_hooks) instead of running a STA of their own.
#include "wifi_coex.h"
#include "zboss_ncp_api.h"
#include "coord_health.h"
#include "coordid.h"
#include "boot_guard.h"
#include "transport.h"
#include "version.h"
#include "sdkconfig.h"

#include <esp_log.h>
#include <string.h>
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include <esp_coexist.h>
#endif

static const char* TAG = "NCPHOST";
static ncp_host_hooks_t s_hooks = {};

extern "C" void ncp_host_set_hooks(const ncp_host_hooks_t* hooks) {
    if (hooks) s_hooks = *hooks;
}

extern "C" esp_err_t wifi_coex_init(void) { return ESP_OK; }  // ESPHome brings up the network

extern "C" void wifi_coex_enable_radio_coex(void) {
    if (!s_hooks.use_wifi) return;
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_wifi_i154_enable();
    ESP_LOGI(TAG, "WiFi/802.15.4 SW coexistence enabled");
#else
    ESP_LOGW(TAG, "CONFIG_ESP_COEX_SW_COEXIST_ENABLE off -- coex NOT enabled");
#endif
}

// Upstream starts WiFi here: after the Zigbee stack is up (SKIP_STARTUP) and
// coexistence is enabled, or straight away in safe mode. ESPHome owns WiFi, so
// this only raises the flag its setup() waits for (zboss_ncp_radio_ready):
// starting WiFi while the 802.15.4 radio initialises hangs in wifi_hw_start
// (task watchdog), which after three boots puts the NCP in safe mode.
static volatile bool s_radio_ready = false;

extern "C" esp_err_t wifi_coex_start_connect(void) {
    ESP_LOGI(TAG, "radio ready (WiFi may start)");
    s_radio_ready = true;
    return ESP_OK;
}

extern "C" bool zboss_ncp_radio_ready(void) { return s_radio_ready; }

// ESPHome initialises WiFi after the go-ahead, i.e. after the first
// esp_coex_wifi_i154_enable(); apply it again once WiFi runs.
extern "C" void ncp_host_reapply_coex(void) {
    if (s_radio_ready && !boot_guard::safe_mode()) wifi_coex_enable_radio_coex();
}

extern "C" bool wifi_coex_is_up(void) {
    return s_hooks.network_up ? s_hooks.network_up() : false;
}

extern "C" bool ncp_tcp_peer_allowed(uint32_t ipv4_be) {
    return s_hooks.peer_allowed ? s_hooks.peer_allowed(ipv4_be) : true;
}

extern "C" void ncp_get_status(ncp_status_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->valid = g_coord_health.valid;
    out->joined = g_coord_health.joined;
    out->pan_id = g_coord_health.pan;
    out->channel = g_coord_health.channel;
    if (!coord_health_read_extpan(out->ext_pan_id)) {
        memset(out->ext_pan_id, 0xFF, sizeof(out->ext_pan_id));
    }
    out->network_lost = g_network_lost;
    out->host_connected = transport::tcp_route_active();
    out->safe_mode = boot_guard::safe_mode();
}

extern "C" const char* zboss_ncp_version(void) { return FW_VERSION_STRING; }

// Only the info page (not in this build) reads these; ESPHome reports IP and RSSI itself.
extern "C" void wifi_coex_current_ip(char* buf, size_t len) {
    if (buf && len) buf[0] = '\0';
}
extern "C" bool wifi_coex_current_rssi(int8_t*) { return false; }
