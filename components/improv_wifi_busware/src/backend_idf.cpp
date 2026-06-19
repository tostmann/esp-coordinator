#if defined(ESP_PLATFORM) && !defined(ARDUINO)

#include "improv_wifi/idf_backend.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace improv_wifi_busware {

static const char* TAG = "improv_wifi_busware";

EspIdfWiFiBackend::EspIdfWiFiBackend() : EspIdfWiFiBackend(Options{}) {}

EspIdfWiFiBackend::EspIdfWiFiBackend(const Options& opts) : opts_(opts) {
    ensureBaseInit_();
}

EspIdfWiFiBackend::~EspIdfWiFiBackend() {
    if (eventsBound_) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEvtTrampoline_);
        esp_event_handler_unregister(IP_EVENT,   IP_EVENT_STA_GOT_IP, &ipEvtTrampoline_);
    }
}

void EspIdfWiFiBackend::ensureBaseInit_() {
    if (baseInit_) return;

    // netif + event loop -- tolerate already-initialized states.
    esp_err_t er = esp_netif_init();
    if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(er));
        return;
    }
    er = esp_event_loop_create_default();
    if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(er));
        return;
    }

    // STA netif -- pick up an existing one if the app already created it.
    staNetif_ = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (staNetif_ == nullptr) {
        staNetif_ = esp_netif_create_default_wifi_sta();
    }

    // WiFi driver init.
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    er = esp_wifi_init(&wcfg);
    if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(er));
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    // Bind events.
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEvtTrampoline_, this);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &ipEvtTrampoline_, this);
    eventsBound_ = true;

    er = esp_wifi_start();
    if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(er));
        return;
    }

    baseInit_ = true;
}

void EspIdfWiFiBackend::wifiEvtTrampoline_(void* arg, esp_event_base_t /*base*/, int32_t id, void* data) {
    static_cast<EspIdfWiFiBackend*>(arg)->onWifiEvent_(id, data);
}

void EspIdfWiFiBackend::ipEvtTrampoline_(void* arg, esp_event_base_t /*base*/, int32_t id, void* data) {
    static_cast<EspIdfWiFiBackend*>(arg)->onIpEvent_(id, data);
}

void EspIdfWiFiBackend::onWifiEvent_(int32_t id, void* data) {
    switch (id) {
        case WIFI_EVENT_STA_CONNECTED:
            connected_ = true;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            connected_     = false;
            gotIp_         = false;
            if (connecting_) connectFailed_ = true;
            break;
        case WIFI_EVENT_SCAN_DONE: {
            scanRunning_ = false;
            uint16_t apCount = 0;
            if (esp_wifi_scan_get_ap_num(&apCount) == ESP_OK && apCount > 0) {
                scanCache_.assign(apCount, {});
                uint16_t got = apCount;
                if (esp_wifi_scan_get_ap_records(&got, scanCache_.data()) == ESP_OK) {
                    scanCount_ = got;
                } else {
                    scanFailed_ = true;
                    scanCount_  = 0;
                }
            } else {
                scanCount_ = 0;
            }
            (void)data;
            break;
        }
        default:
            break;
    }
}

void EspIdfWiFiBackend::onIpEvent_(int32_t id, void* data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        gotIp_ = true;
        (void)data;
    }
}

bool EspIdfWiFiBackend::isConnected() {
    return connected_.load() && gotIp_.load();
}

std::string EspIdfWiFiBackend::currentIp() {
    if (!gotIp_.load() || !staNetif_) return {};
    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(staNetif_, &ip) != ESP_OK) return {};
    if (ip.ip.addr == 0) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  static_cast<unsigned>(esp_ip4_addr1_16(&ip.ip)),
                  static_cast<unsigned>(esp_ip4_addr2_16(&ip.ip)),
                  static_cast<unsigned>(esp_ip4_addr3_16(&ip.ip)),
                  static_cast<unsigned>(esp_ip4_addr4_16(&ip.ip)));
    return std::string(buf);
}

bool EspIdfWiFiBackend::tryConnect(const char* ssid, const char* password) {
    if (!baseInit_ || !ssid) return false;

    wifi_config_t wcfg{};
    std::strncpy(reinterpret_cast<char*>(wcfg.sta.ssid),
                 ssid,
                 sizeof(wcfg.sta.ssid) - 1);
    if (password && *password) {
        std::strncpy(reinterpret_cast<char*>(wcfg.sta.password),
                     password,
                     sizeof(wcfg.sta.password) - 1);
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    wcfg.sta.pmf_cfg.capable  = true;
    wcfg.sta.pmf_cfg.required = false;
    if (opts_.connectToStrongest) {
        // Make the WiFi driver scan all channels before associating and pick
        // the strongest matching BSSID. Without this, the IDF default is
        // WIFI_FAST_SCAN, which connects to the *first* matching BSSID seen.
        wcfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wcfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    }

    // Drop any previous association cleanly. We zero connecting_ FIRST so the
    // STA_DISCONNECTED event that the WiFi driver fires for the old link does
    // not get misclassified as a failure of the new connect we are about to
    // start (the disconnect handler only flags connectFailed_ while
    // connecting_ is true). Then wait for the driver to actually leave the
    // connected state before reconfiguring -- without this, the wifi driver
    // logs "wifi:sta is connecting, return error" and the new connect attempt
    // can get stuck in a half-baked state during re-provisioning.
    connecting_ = false;
    esp_wifi_disconnect();
    for (int i = 0; i < 50 && connected_.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (esp_wifi_set_config(WIFI_IF_STA, &wcfg) != ESP_OK) return false;

    connected_     = false;
    gotIp_         = false;
    connectFailed_ = false;
    connecting_    = true;
    if (esp_wifi_connect() != ESP_OK) {
        connecting_ = false;
        return false;
    }

    // Wait loop. Allow exactly one retry on connect failure -- some routers
    // reject the first probe right after a fresh disconnect. After two
    // consecutive failures we give up rather than cascade-retry until the
    // deadline, which previously kept the lib unresponsive for the full
    // 12 s connectTimeoutMs even when the real outcome was already known.
    bool retried = false;
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(opts_.connectTimeoutMs) * 1000;
    while (esp_timer_get_time() < deadline) {
        if (connected_.load() && gotIp_.load()) {
            connecting_ = false;
            return true;
        }
        if (connectFailed_.load()) {
            connectFailed_ = false;
            if (retried) {
                connecting_ = false;
                return false;
            }
            retried = true;
            esp_wifi_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    connecting_ = false;
    return connected_.load() && gotIp_.load();
}

void EspIdfWiFiBackend::startScan() {
    if (!baseInit_) return;
    scanCache_.clear();
    scanCount_   = 0;
    scanFailed_  = false;
    scanRunning_ = true;

    wifi_scan_config_t cfg{};
    cfg.show_hidden       = false;
    cfg.scan_type         = WIFI_SCAN_TYPE_ACTIVE;
    if (opts_.maxScanTimeMs > 0) {
        cfg.scan_time.active.max = opts_.maxScanTimeMs;
    }
    const esp_err_t er = esp_wifi_scan_start(&cfg, /*block=*/false);
    if (er != ESP_OK) {
        scanRunning_ = false;
        scanFailed_  = true;
    }
}

int EspIdfWiFiBackend::scanResult() {
    if (scanRunning_.load()) return WiFiBackend::kScanRunning;
    if (scanFailed_.load())  return WiFiBackend::kScanFailed;
    return scanCount_.load();
}

ApRecord EspIdfWiFiBackend::apRecord(int index) {
    ApRecord out;
    if (index < 0 || static_cast<size_t>(index) >= scanCache_.size()) return out;
    const auto& rec = scanCache_[static_cast<size_t>(index)];
    out.ssid.assign(reinterpret_cast<const char*>(rec.ssid));
    out.rssi      = rec.rssi;
    out.needsAuth = (rec.authmode != WIFI_AUTH_OPEN);
    return out;
}

void EspIdfWiFiBackend::clearScan() {
    scanCache_.clear();
    scanCount_   = 0;
    scanFailed_  = false;
    scanRunning_ = false;
}

}  // namespace improv_wifi_busware

#endif  // ESP_PLATFORM && !ARDUINO
