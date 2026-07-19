#include "coordid.h"

#include <nvs.h>
#include <cstring>

// Sole owner of the definition (coord_health.h only extern-declares it).
volatile bool g_network_lost = false;

namespace {
constexpr char NS[]  = "coordid";
constexpr char KEY[] = "id";
}  // namespace

bool coordid_load(coord_identity_t *out) {
    if (!out) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;  // namespace never created -> no marker
    }
    size_t n = sizeof(*out);
    esp_err_t e = nvs_get_blob(h, KEY, out, &n);
    nvs_close(h);
    return e == ESP_OK && n == sizeof(*out);
}

esp_err_t coordid_save(const coord_identity_t *in) {
    if (!in) return ESP_ERR_INVALID_ARG;
    coord_identity_t cur;
    if (coordid_load(&cur) && memcmp(&cur, in, sizeof(cur)) == 0) {
        return ESP_OK;  // unchanged -> skip the write (flash wear)
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, KEY, in, sizeof(*in));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t coordid_clear(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;  // nothing to clear
    if (e != ESP_OK) return e;
    esp_err_t er = nvs_erase_key(h, KEY);  // NOT_FOUND if never written — benign
    if (er != ESP_OK && er != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return er;
    }
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}
