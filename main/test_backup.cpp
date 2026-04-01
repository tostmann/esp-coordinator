#include "esp_zigbee_core.h"
#include "zboss_api.h"
#include "nvs.h"

extern "C" uint8_t* secur_nwk_key_by_seq(uint8_t seq);

void fill_backup() {
    uint8_t* key = secur_nwk_key_by_seq(0);
    uint16_t pan = esp_zb_get_pan_id();
    uint8_t channel = esp_zb_get_current_channel();
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("zboss", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        size_t required_size = 0;
        if (nvs_get_blob(my_handle, "2", NULL, &required_size) == ESP_OK) {
            // NVS dataset 2 has frame counter inside
        }
        nvs_close(my_handle);
    }
}
