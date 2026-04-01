#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

extern "C" void test_zboss_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("zboss", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        printf("BACKUP: Error opening NVS handle!\n");
        return;
    }

    size_t required_size = 0;
    err = nvs_get_blob(my_handle, "4", NULL, &required_size);
    if (err == ESP_OK && required_size > 0) {
        uint8_t* sec_blob = (uint8_t*)malloc(required_size);
        nvs_get_blob(my_handle, "4", sec_blob, &required_size);
        printf("BACKUP SEC BLOB SIZE: %d\n", required_size);
        ESP_LOG_BUFFER_HEX("NVS_SEC_BLOB", sec_blob, required_size);
        free(sec_blob);
    } else {
        printf("BACKUP: NO SEC BLOB\n");
    }

    required_size = 0;
    err = nvs_get_blob(my_handle, "2", NULL, &required_size);
    if (err == ESP_OK && required_size > 0) {
        uint8_t* nwk_blob = (uint8_t*)malloc(required_size);
        nvs_get_blob(my_handle, "2", nwk_blob, &required_size);
        printf("BACKUP NWK BLOB SIZE: %d\n", required_size);
        ESP_LOG_BUFFER_HEX("NVS_NWK_BLOB", nwk_blob, required_size);
        free(nwk_blob);
    } else {
        printf("BACKUP: NO NWK BLOB\n");
    }

    nvs_close(my_handle);
}
