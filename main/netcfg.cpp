#include "netcfg.h"

#include <nvs.h>
#include <esp_log.h>
#include <cstring>

static const char *TAG = "NETCFG";

namespace {
constexpr char NS[]       = "netcfg";
constexpr char KEY_SSID[] = "sta_ssid";
constexpr char KEY_PSK[]  = "sta_psk";
}  // namespace

bool netcfg_has_sta_creds(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;  // namespace never created -> no creds
    }
    char ssid[NETCFG_SSID_MAXLEN] = {0};
    size_t len = sizeof(ssid);
    esp_err_t err = nvs_get_str(h, KEY_SSID, ssid, &len);
    nvs_close(h);
    return (err == ESP_OK && ssid[0] != '\0');
}

esp_err_t netcfg_load(char *ssid, size_t ssid_len, char *psk, size_t psk_len) {
    if (!ssid || !psk || ssid_len == 0 || psk_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    psk[0]  = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = ssid_len;
    err = nvs_get_str(h, KEY_SSID, ssid, &len);
    if (err != ESP_OK || ssid[0] == '\0') {
        nvs_close(h);
        ssid[0] = '\0';
        return (err == ESP_OK) ? ESP_ERR_NVS_NOT_FOUND : err;
    }

    // PSK is optional (open network) — a missing key is not fatal.
    len = psk_len;
    esp_err_t perr = nvs_get_str(h, KEY_PSK, psk, &len);
    if (perr != ESP_OK) {
        psk[0] = '\0';
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t netcfg_persist(const char *ssid, const char *psk) {
    if (!ssid || ssid[0] == '\0' || strlen(ssid) >= NETCFG_SSID_MAXLEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (psk && strlen(psk) >= NETCFG_PSK_MAXLEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_PSK, psk ? psk : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "STA creds persisted for SSID '%s'", ssid);
    }
    return err;
}

esp_err_t netcfg_clear(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  // nothing to clear
    }
    if (err != ESP_OK) {
        return err;
    }
    // erase_key returns NOT_FOUND if the key was never written — benign.
    esp_err_t e1 = nvs_erase_key(h, KEY_SSID);
    esp_err_t e2 = nvs_erase_key(h, KEY_PSK);
    if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) err = e1;
    if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) err = e2;
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
