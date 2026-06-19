#include "improv_provisioning.h"
#include "netcfg.h"
#include "wifi_coex.h"
#include "transport.h"
#include "version.h"

#include "improv_wifi/improv_wifi.h"
#include "improv_wifi/idf_backend.h"
#include "improv_wifi/wifi_backend.h"
#include "improv_wifi/types.h"

#include <driver/usb_serial_jtag.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <string>

static const char *TAG = "IMPROV";
namespace ipw = improv_wifi_busware;

namespace {
// Mode A = first-time provisioning (no creds): live WiFi connect via the IDF
// backend, window open for the whole session. Mode B = reconfig window (creds
// already present, wifi_coex owns the STA): capture-only, bounded 120 s, then
// the task frees itself and USB-Serial/JTAG goes idle.
enum class Mode { ProvisionA, ReconfigB };
Mode             s_mode         = Mode::ProvisionA;
ipw::ImprovWiFi *s_inst         = nullptr;
int64_t          s_reboot_at_us = 0;  // 0 = no reboot scheduled

void write_fn(const uint8_t *data, size_t len, void *) {
    if (!data || len == 0) return;
    usb_serial_jtag_write_bytes(reinterpret_cast<const char *>(data), len, pdMS_TO_TICKS(50));
}

void on_error_cb(ipw::Error e, void *) {
    ESP_LOGW(TAG, "improv error 0x%02x", static_cast<unsigned>(e));
}

void on_connected_cb(const char *ssid, const char *psk, void *) {
    // Mode A only: a fresh device just got its first creds via a live connect.
    // Persist + reboot into Mode B. (Mode B reconfig persists+reboots inside
    // CaptureBackend::tryConnect, so it sets onConnected = nullptr.)
    ESP_LOGI(TAG, "provisioned to '%s' — persisting + rebooting into Mode B",
             ssid ? ssid : "?");
    netcfg_persist(ssid ? ssid : "", psk ? psk : "");
    // Defer the restart ~3 s so the lib's Provisioned + device-URL frames flush
    // to the web flasher before the USB endpoint drops on esp_restart().
    s_reboot_at_us = esp_timer_get_time() + 3'000'000;
}

// Mode B reconfig backend: deliberately NO esp_wifi_* calls. In Mode B
// wifi_coex owns the STA — its WIFI_EVENT_STA_DISCONNECTED auto-reconnect
// (to the *configured* creds) and the link watchdog would fight a live
// re-connect, and an AP scan would force the shared 2.4 GHz radio off the
// Zigbee channel and drop the coordinator link. So we CAPTURE the new creds
// and reboot; wifi_coex_init() applies + validates them on the next boot (and
// its watchdog reboots — reopening this window — on a bad password).
class CaptureBackend : public ipw::WiFiBackend {
public:
    // Always report "not connected" so the flasher presents STATE_AUTHORIZED
    // and prompts for WiFi — this is an intentional reconfig, not a status read.
    bool isConnected() override { return false; }

    std::string currentIp() override {
        char ip[16] = {0};
        wifi_coex_current_ip(ip, sizeof(ip));  // real current IP for the cosmetic post-success URL
        return ip;
    }

    bool tryConnect(const char *ssid, const char *password) override {
        if (!ssid || ssid[0] == '\0') return false;
        if (netcfg_persist(ssid, password ? password : "") != ESP_OK) return false;
        ESP_LOGI(TAG, "reconfig: new STA creds for '%s' captured — rebooting", ssid);
        // Let the Provisioned + device-URL frames flush, then reboot; the new
        // creds take effect in wifi_coex_init() on the next boot.
        s_reboot_at_us = esp_timer_get_time() + 3'000'000;
        return true;
    }

    // No scan: returning 0 makes the flasher offer manual SSID entry without
    // taking the radio off the Zigbee channel (which a real scan would do).
    void          startScan() override {}
    int           scanResult() override { return 0; }
    ipw::ApRecord apRecord(int) override { return {}; }
    void          clearScan() override {}
};
CaptureBackend s_capture_backend;

// Bring WiFi up eagerly with WIFI_PS_NONE so the C6 4-way-handshake works during
// the Improv backend's tryConnect (the backend does not set power-save itself).
// Mode A only — in Mode B wifi_coex_init() already created the netif/STA, and
// calling this again would create a second default STA netif. All steps tolerate
// "already done" — EspIdfWiFiBackend::ensureBaseInit_ is idempotent.
void prime_wifi_for_c6() {
    esp_netif_init();
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(e));
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "wifi_init: %s", esp_err_to_name(e));
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);  // C6 rule
    esp_wifi_start();
}

void improv_task(void *) {
    uint8_t buf[64];
    while (true) {
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        s_inst->tick(now_ms);

        int got = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (got > 0) {
            s_inst->feedBytes(buf, static_cast<size_t>(got));
        }

        if (s_reboot_at_us != 0 && esp_timer_get_time() >= s_reboot_at_us) {
            ESP_LOGI(TAG, "rebooting");
            // FIN any connected TCP client (Mode B) before the reboot drops
            // WiFi — otherwise z2m hangs on a dead socket until keepalive
            // timeout. No-op in Mode A (use_tcp never set). 100 ms lets the
            // shutdown's FIN reach the wire while WiFi is still up. Mirrors the
            // NCP_RESET / RESTORE_NETWORK reboot paths (commands_impl.h).
            transport::close_tcp_client();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }

        // Mode B: once the bounded reconfig window has closed (and no reboot is
        // pending) stop owning USB-Serial/JTAG and free this task — in Mode B
        // the endpoint then sits idle while the NCP runs over TCP/UART. Mode A
        // keeps looping (its window spans the whole provisioning session and
        // nothing else uses the endpoint until a reboot into Mode B).
        if (s_mode == Mode::ReconfigB && s_reboot_at_us == 0 && !s_inst->isArmed()) {
            ESP_LOGI(TAG, "reconfig window closed — Improv idle");
            s_inst = nullptr;
            vTaskDelete(NULL);
        }
    }
}

esp_err_t start_task(ipw::ImprovWiFi *inst) {
    s_inst = inst;
    BaseType_t r = xTaskCreate(&improv_task, "improv", 4096, nullptr, 5, nullptr);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "improv task create failed");
        s_inst = nullptr;
        return ESP_FAIL;
    }
    return ESP_OK;
}
}  // namespace

extern "C" esp_err_t improv_provisioning_start(void) {
    s_mode = Mode::ProvisionA;
    prime_wifi_for_c6();

    static ipw::EspIdfWiFiBackend be;

    ipw::Config c;
    c.backend                = &be;
    c.write                  = &write_fn;
    c.userCtx                = nullptr;
    // No NCP role in Mode A — keep the window effectively open until the device
    // is provisioned (then it reboots into Mode B).
    c.windowMs               = 60u * 60u * 1000u;
    c.device.chipFamily      = ipw::ChipFamily::Esp32C6;
    c.device.firmwareName    = "esp-coordinator";
    c.device.firmwareVersion = FW_VERSION_STRING;
    c.device.deviceName      = "busware ESP32 ZBOSS";
    c.device.deviceUrl       = nullptr;  // lib fills http://<ip>/
    c.onError                = &on_error_cb;
    c.onConnected            = &on_connected_cb;

    static ipw::ImprovWiFi inst{c};
    esp_err_t err = start_task(&inst);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Improv-Serial provisioning armed (Mode A) on USB-Serial/JTAG");
    }
    return err;
}

extern "C" esp_err_t improv_provisioning_start_reconfig(void) {
    s_mode = Mode::ReconfigB;
    // NO prime_wifi_for_c6() — wifi_coex_init() already owns the netif + STA.

    ipw::Config c;
    c.backend                = &s_capture_backend;
    c.write                  = &write_fn;
    c.userCtx                = nullptr;
    // Bounded 120 s reconfig window (Dirk: "Improv should run 120 s after boot
    // in any case, to reconfigure even when WiFi is up").
    c.windowMs               = 120u * 1000u;
    c.device.chipFamily      = ipw::ChipFamily::Esp32C6;
    c.device.firmwareName    = "esp-coordinator";
    c.device.firmwareVersion = FW_VERSION_STRING;
    c.device.deviceName      = "busware ESP32 ZBOSS";
    c.device.deviceUrl       = nullptr;
    c.onError                = &on_error_cb;
    c.onConnected            = nullptr;  // CaptureBackend::tryConnect persists + reboots

    static ipw::ImprovWiFi inst{c};
    esp_err_t err = start_task(&inst);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Improv-Serial reconfig window armed (Mode B, 120 s) on USB-Serial/JTAG");
    }
    return err;
}
