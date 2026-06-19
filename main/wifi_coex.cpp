#include "wifi_coex.h"
#include "netcfg.h"
#include "sdkconfig.h"

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <esp_coexist.h>
#include <mdns.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <cstdio>

static const char *TAG = "WIFICOEX";

namespace {
bool          s_inited       = false;
volatile bool s_got_ip       = false;
bool          s_mdns_started = false;
esp_netif_t  *s_sta_netif    = nullptr;

// mDNS: advertise a discoverable hostname + the ZHA network-coordinator service.
// Hostname  esp-coordinator-XXXX.local  (XXXX = last 2 bytes of the STA MAC) is
// the reliable manual path — a Z2M user sets port: tcp://esp-coordinator-XXXX.local:<port>.
// The _zigbee-coordinator._tcp service (radio_type=zboss) is the ZHA zeroconf
// discovery type; the exact TXT schema is to be verified against ZHA/SLZB before
// relying on auto-discovery.
void start_mdns_once() {
    if (s_mdns_started) return;

    // Fixed, MAC-independent hostname so a user can reach the coordinator
    // without ever knowing its MAC: port: tcp://esp-zboss-coo.local:<port>.
    // mDNS auto-disambiguates a 2nd device on the same LAN to
    // esp-zboss-coo-2.local, so a fixed base name is safe for the typical
    // single-coordinator case and far friendlier than a MAC suffix.
    const char *host = "esp-zboss-coord";  // tcp://esp-zboss-coord.local:<port>

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(host);
    mdns_instance_name_set("busware ESP32 ZBOSS Coordinator");

    mdns_txt_item_t txt[] = {
        {"radio_type", "zboss"},
        {"version", "1"},
    };
    mdns_service_add(nullptr, "_zigbee-coordinator", "_tcp",
                     CONFIG_NCP_TCP_PORT, txt, sizeof(txt) / sizeof(txt[0]));

    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS: %s.local + _zigbee-coordinator._tcp:%d", host, CONFIG_NCP_TCP_PORT);
}

// --- WiFi link watchdog ------------------------------------------------
// 802.15.4 bursts (device joins: beacon/assoc/transport-key churn) can starve
// WiFi into a ZOMBIE state: the driver still believes it is associated, but
// the link is ARP/ICMP-dead for minutes and WIFI_EVENT_STA_DISCONNECTED never
// fires (observed 3x on 2026-06-05 — every Zigbee device join killed the
// link until a manual reset). Event-based reconnect is blind to this, so we
// actively probe the default gateway with ICMP and force a disconnect/
// reconnect cycle when the link goes silent; if cycling doesn't recover the
// link, reboot (network state persists in NVRAM, a reboot self-heals).
// Probe = TCP connect to the gateway, NOT ICMP. Lesson from the first
// (esp_ping) implementation: ICMP echo never got a single reply on this
// device even on demonstrably healthy links (TCP flowing, LQI fine) —
// 100% deterministic probe failure, so the watchdog itself killed every
// healthy link 33 s after GOT_IP. A TCP SYN is retransmitted by the stack
// (rides out coex/PS latency spikes), and BOTH outcomes prove the link:
// SYN-ACK (gateway service) or RST (refused) — only a TIMEOUT is a miss.
constexpr uint32_t WDOG_PROBE_INTERVAL_MS = 15000;
constexpr uint32_t WDOG_PROBE_TIMEOUT_S   = 8;
constexpr uint16_t WDOG_PROBE_PORT        = 80;  // verified open on lab gw; RST would do too
constexpr uint32_t WDOG_FAIL_THRESHOLD    = 4;   // ~90 s dead air => force reconnect
constexpr uint32_t WDOG_CYCLE_LIMIT       = 3;   // reconnects w/o recovery => reboot

// No-IP guard (v1.2.42): the WiFi zombie exists PRE-IP too — observed
// 2026-06-05: assoc completed ("connected with ..."), then GOT_IP never fired
// and not a single further wifi event arrived for 40+ min. The TCP-probe
// watchdog used to be armed only at GOT_IP, so that state was invisible to
// it. The guard runs in the same task (started unconditionally at
// wifi_coex_init now): stuck without an IP for KICK_MS => force a
// disconnect (the STA_DISCONNECTED handler re-connects); without a single
// probe- or IP-success for REBOOT_MS => esp_restart() (network state
// persists in NVRAM, a reboot self-heals).
constexpr uint32_t WDOG_NOIP_KICK_MS   = 3 * 60 * 1000;
constexpr uint32_t WDOG_NOIP_REBOOT_MS = 10 * 60 * 1000;

volatile uint32_t      s_wdog_fails  = 0;
volatile uint32_t      s_wdog_cycles = 0;
volatile uint32_t      s_gw_addr     = 0;  // network byte order; 0 = unknown
TaskHandle_t           s_wdog_task   = nullptr;
volatile TickType_t    s_last_success_tick = 0;  // last probe success or GOT_IP
volatile TickType_t    s_noip_since_tick   = 0;  // when s_got_ip last went false

bool wdog_probe_once(uint32_t gw_addr) {
    int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return true;  // out of sockets != link dead; don't punish the link
    int fl = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
    sockaddr_in dst = {};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(WDOG_PROBE_PORT);
    dst.sin_addr.s_addr = gw_addr;
    bool alive = false;
    int r = ::connect(s, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    if (r == 0) {
        alive = true;
    } else if (errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        timeval tv = {.tv_sec = WDOG_PROBE_TIMEOUT_S, .tv_usec = 0};
        if (::select(s + 1, nullptr, &wf, nullptr, &tv) > 0) {
            int err = 0;
            socklen_t l = sizeof(err);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
            // SYN-ACK (err==0) and RST (ECONNREFUSED/ECONNRESET) both prove L3.
            alive = (err == 0 || err == ECONNREFUSED || err == ECONNRESET);
        }
    } else if (errno == ECONNREFUSED || errno == ECONNRESET) {
        alive = true;
    }
    ::close(s);
    return alive;
}

void wdog_task_fn(void *) {
    s_last_success_tick = xTaskGetTickCount();
    s_noip_since_tick   = xTaskGetTickCount();
    uint32_t noip_kicks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WDOG_PROBE_INTERVAL_MS));
        const TickType_t now = xTaskGetTickCount();

        // Hard backstop, state-independent: nothing has proven the link
        // (probe success or fresh IP) for REBOOT_MS => reboot. Catches the
        // pre-IP zombie AND any post-IP state the cycle logic failed to heal.
        if ((now - s_last_success_tick) >= pdMS_TO_TICKS(WDOG_NOIP_REBOOT_MS)) {
            ESP_LOGE(TAG, "link watchdog: no IP/probe success for %u min — rebooting",
                     (unsigned)(WDOG_NOIP_REBOOT_MS / 60000));
            esp_restart();
        }

        uint32_t gw = s_gw_addr;
        if (!s_got_ip || gw == 0) {
            // No-IP guard: stuck pre-IP (associated-but-dead, DHCP black hole,
            // or assoc loop) for KICK_MS => force a disconnect; the
            // STA_DISCONNECTED handler re-connects. Repeat every KICK_MS.
            if ((now - s_noip_since_tick) >= pdMS_TO_TICKS(WDOG_NOIP_KICK_MS) * (noip_kicks + 1)) {
                ++noip_kicks;
                ESP_LOGE(TAG, "link watchdog: no IP for %u min — kicking WiFi (disconnect/reconnect)",
                         (unsigned)(noip_kicks * WDOG_NOIP_KICK_MS / 60000));
                esp_wifi_disconnect();
            }
            continue;
        }
        noip_kicks = 0;
        if (wdog_probe_once(gw)) {
            s_wdog_fails  = 0;
            s_wdog_cycles = 0;  // genuine recovery only counts when the gateway answers
            s_last_success_tick = xTaskGetTickCount();
            continue;
        }
        if (!s_got_ip) continue;  // disconnect raced the probe
        if (++s_wdog_fails < WDOG_FAIL_THRESHOLD) continue;
        s_wdog_fails = 0;
        ++s_wdog_cycles;
        if (s_wdog_cycles >= WDOG_CYCLE_LIMIT) {
            ESP_LOGE(TAG, "link watchdog: %u reconnect cycles without recovery — rebooting",
                     (unsigned)s_wdog_cycles);
            esp_restart();
        }
        ESP_LOGE(TAG, "link watchdog: gateway TCP-silent %u probes — forcing reconnect (cycle %u/%u)",
                 (unsigned)WDOG_FAIL_THRESHOLD, (unsigned)s_wdog_cycles, (unsigned)WDOG_CYCLE_LIMIT);
        s_noip_since_tick = xTaskGetTickCount();  // refresh here: the evt handler
        s_got_ip = false;                         // sees s_got_ip already false
        esp_wifi_disconnect();  // STA_DISCONNECTED handler performs the reconnect
    }
}

// The watchdog task itself is started unconditionally at wifi_coex_init (the
// no-IP guard must run BEFORE the first GOT_IP — that's the v1.2.41 gap that
// left the pre-IP zombie unguarded). This arm step only publishes the
// gateway address for the TCP probe.
void wdog_arm(esp_ip4_addr_t gw) {
    s_gw_addr    = gw.addr;
    s_wdog_fails = 0;
    if (gw.addr == 0) {
        ESP_LOGW(TAG, "link watchdog: no gateway address, TCP probe idle (no-IP guard still active)");
        return;
    }
    ESP_LOGI(TAG, "link watchdog armed: TCP-probe " IPSTR ":%u, %u s interval",
             IP2STR(&gw), (unsigned)WDOG_PROBE_PORT, (unsigned)(WDOG_PROBE_INTERVAL_MS / 1000));
}

void wifi_evt(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_got_ip) {
            s_noip_since_tick = xTaskGetTickCount();  // a fresh no-IP episode begins
        }
        s_got_ip = false;  // TCP probe idles while this is false
        esp_wifi_connect();  // bounded auto-reconnect
    }
}

void ip_evt(void *, esp_event_base_t, int32_t id, void *data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        s_got_ip = true;
        s_last_success_tick = xTaskGetTickCount();  // an IP is link proof
        start_mdns_once();
        const ip_event_got_ip_t *e = static_cast<const ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "got IP " IPSTR " — NCP-over-TCP available", IP2STR(&e->ip_info.ip));
        wdog_arm(e->ip_info.gw);
    }
}
}  // namespace

extern "C" esp_err_t wifi_coex_init(void) {
    if (s_inited) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_evt, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // C6 rule: WIFI_PS_NONE before connect (else WPA2 4-way-handshake timeout).
    // Coex tradeoff: NONE keeps WiFi always-on so the shared 2.4 GHz radio never
    // yields to 802.15.4 RX — worst case for an always-RX Zigbee coordinator.
    // esp_zigbee_gateway uses WIFI_PS_MIN_MODEM for coex headroom; bench both on
    // HW (Espressif rates this WiFi-STA x coordinator case C1/unstable).
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    char ssid[NETCFG_SSID_MAXLEN] = {0};
    char psk[NETCFG_PSK_MAXLEN] = {0};
    if (netcfg_load(ssid, sizeof(ssid), psk, sizeof(psk)) != ESP_OK) {
        ESP_LOGE(TAG, "no creds at wifi_coex_init (unexpected in Mode B)");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char *>(wc.sta.ssid), ssid, sizeof(wc.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wc.sta.password), psk, sizeof(wc.sta.password) - 1);
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    // v1.2.42: the watchdog task runs from init on, not from first GOT_IP —
    // its no-IP guard is what covers the window before (or forever without)
    // an IP. The TCP probe inside it self-gates on s_got_ip && s_gw_addr.
    if (!s_wdog_task) {
        xTaskCreate(wdog_task_fn, "lnkwdog", 3072, nullptr, 2, &s_wdog_task);
    }

    s_inited = true;
    ESP_LOGI(TAG, "wifi_coex_init: STA configured for SSID '%s'", ssid);
    return ESP_OK;
}

extern "C" void wifi_coex_enable_radio_coex(void) {
    if (!s_inited) return;
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_wifi_i154_enable();
    ESP_LOGI(TAG, "WiFi/802.15.4 SW coexistence enabled");
#else
    ESP_LOGW(TAG, "CONFIG_ESP_COEX_SW_COEXIST_ENABLE off — coex NOT enabled");
#endif
}

extern "C" esp_err_t wifi_coex_start_connect(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(e));
    return e;
}

extern "C" bool wifi_coex_is_up(void) { return s_got_ip; }

extern "C" void wifi_coex_current_ip(char *buf, size_t len) {
    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (!s_got_ip || !s_sta_netif) return;
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
}
