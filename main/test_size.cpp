#include "esp_zigbee_core.h"
#include "zboss_api.h"
#include <stdio.h>

void test_size() {
    esp_zb_overall_network_size_set(250);
    ZB_INIT();
    zb_set_max_children(100);
}
