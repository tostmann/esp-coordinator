#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_partition.h"
#include "app.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// NCP_RESET options 1/2 (NVRAM erase / factory reset) cannot erase flash
// inline: any multi-sector flash erase while the 802.15.4 radio is active
// freezes the chip (cache-disabled stall — observed live 2026-06-05 with
// both zb_nvram_erase() and a raw esp_partition_erase_range from the
// rst_dly task; the whole NCP pump dies, only a power-cycle recovers).
// Instead the reset handler parks the request in RTC-noinit RAM (survives
// esp_restart(), zero flash ops pre-reset) and we execute the erase HERE,
// before the ZBOSS stack / radio starts and flash access is uncontended.
RTC_NOINIT_ATTR uint32_t g_pending_ncp_erase;
#define NCP_ERASE_MAGIC 0xFAC70000u  // low byte = reset options

static void handle_pending_erase(void)
{
    if ((g_pending_ncp_erase & 0xFFFFFF00u) != NCP_ERASE_MAGIC) {
        g_pending_ncp_erase = 0;  // power-on garbage — normalize
        return;
    }
    uint8_t options = g_pending_ncp_erase & 0xFFu;
    g_pending_ncp_erase = 0;
    if (options == 1 || options == 2) {
        const esp_partition_t* zb = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "zb_storage");
        if (zb) esp_partition_erase_range(zb, 0, zb->size);
    }
    if (options == 2) {
        const esp_partition_t* nvs = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        if (nvs) esp_partition_erase_range(nvs, 0, nvs->size);
    }
}

#if CONFIG_NCP_XIAO_EXT_ANTENNA
// Seeed XIAO ESP32-C6 RF antenna switch (pins NOT on the module header):
// GPIO3 low enables the switch control, GPIO14 selects the antenna
// (low = internal ceramic, high = external U.FL). Sequence and the 100 ms
// settle delay follow Seeed's reference code (wiki "XIAO ESP32C6 Getting
// Started"; pin names WIFI_ENABLE/WIFI_ANT_CONFIG in arduino-esp32
// variants/XIAO_ESP32C6/pins_arduino.h). Runs before the ZBOSS stack starts,
// so the radio only ever transmits on the selected antenna.
static void select_external_antenna(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_14);
    // INPUT_OUTPUT (not plain OUTPUT) so the level can be read back for the
    // debug log below — push-pull drive is identical.
    io.mode = GPIO_MODE_INPUT_OUTPUT;
    gpio_config(&io);
    gpio_set_level(GPIO_NUM_3, 0);   // enable RF switch control
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_NUM_14, 1);  // external antenna
    ESP_LOGI("ANT", "ext antenna selected: GPIO3=%d GPIO14=%d",
             gpio_get_level(GPIO_NUM_3), gpio_get_level(GPIO_NUM_14));
}
#else
// Default build: put the (XIAO) RF-switch control pins into a defined,
// passive state — inputs, no pulls. On the XIAO the board's 5K1 pulldown on
// GPIO14 then selects RF1 = the on-board chip antenna (schematic-verified
// 2026-06-05), and GPIO3 stays high-Z (switch control inactive). On any
// other board this matches the chip's reset state, i.e. it is electrically
// a no-op — safe in the one-binary-for-all webflasher image.
static void rf_switch_default_input(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_14);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);
}
#endif

extern "C" void app_main(void)
{
    // Reset-cause forensics (free under LOG_NONE): some boards' eFuses
    // suppress the ROM's rst:0x banner, making brownouts, panics and
    // watchdog resets indistinguishable from clean boots on a console.
    // ESP_RST_: 1=poweron 3=sw 4=panic 5=int_wdt 6=task_wdt 7=other_wdt
    // 9=brownout 11=usb.
    ESP_LOGI("BOOT", "reset reason: %d", (int)esp_reset_reason());

    handle_pending_erase();
#if CONFIG_NCP_XIAO_EXT_ANTENNA
    select_external_antenna();
#else
    rf_switch_default_input();
#endif
    ESP_ERROR_CHECK(app::init());
    ESP_ERROR_CHECK(app::start());
}
