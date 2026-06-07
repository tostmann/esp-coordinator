#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "app.h"
#include "boot_guard.h"
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

// WEDGE-1 boot guard implementation — see boot_guard.h for the design.
// Breadcrumb layout: bits[31:8] = magic 0xB007BC, bits[7:0] = consecutive
// early-boot failure count. RTC-noinit survives esp_restart / watchdog
// resets; after a true power-on the magic check fails and we start fresh
// (a power cycle deserves a clean ZBOSS attempt anyway).
RTC_NOINIT_ATTR static uint32_t g_boot_breadcrumb;
#define BOOT_BC_MAGIC      0xB007BC00u
#define BOOT_BC_MAGIC_MASK 0xFFFFFF00u
#define BOOT_FAIL_LIMIT    3
#define BOOT_DEADLINE_US   (30 * 1000 * 1000)

namespace boot_guard {

static bool s_safe_mode = false;
static esp_timer_handle_t s_deadline = nullptr;

static void deadline_cb(void*)
{
    // ZBOSS never reached SKIP_STARTUP within the deadline: the early-init
    // hang class. Reboot so the failure count can accumulate; after
    // BOOT_FAIL_LIMIT of these the next boot enters safe mode.
    ESP_LOGE("BOOT", "boot guard: ZBOSS init deadline expired, restarting");
    esp_restart();
}

void init()
{
    uint32_t fails = 0;
    if ((g_boot_breadcrumb & BOOT_BC_MAGIC_MASK) == BOOT_BC_MAGIC) {
        fails = g_boot_breadcrumb & 0xFFu;
    }
    ESP_LOGI("BOOT", "boot guard: early-boot failure count %u", (unsigned)fails);

    if (fails >= BOOT_FAIL_LIMIT) {
        // Safe mode is STICKY: the count is kept, so warm resets (including
        // the reset a host's serial-port open pulses on USB-Serial-JTAG)
        // land right back here — a stable, diagnosable state. Exits:
        //  - host NCP_RESET command (supervised restart; clears the count,
        //    optionally factory-erasing the NVRAM the stack chokes on),
        //  - a true power cycle (RTC magic check fails),
        //  - the 10 min self-heal timer below (unattended sticks retry a
        //    normal start eventually in case the cause was transient).
        s_safe_mode = true;
        ESP_LOGE("BOOT", "boot guard: entering SAFE MODE (no ZBOSS)");
        const esp_timer_create_args_t heal_args = {
            .callback = [](void*) { boot_guard::clear(); esp_restart(); },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "bootheal",
            .skip_unhandled_events = true,
        };
        esp_timer_handle_t heal = nullptr;
        if (esp_timer_create(&heal_args, &heal) == ESP_OK) {
            esp_timer_start_once(heal, 10ULL * 60 * 1000 * 1000);
        }
        return;
    }

    // Assume failure; mark_zboss_alive() clears this when the stack is up.
    g_boot_breadcrumb = BOOT_BC_MAGIC | (fails + 1u);

    const esp_timer_create_args_t args = {
        .callback = deadline_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bootguard",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_deadline) == ESP_OK) {
        esp_timer_start_once(s_deadline, BOOT_DEADLINE_US);
    }
}

bool safe_mode()
{
    return s_safe_mode;
}

void mark_zboss_alive()
{
    g_boot_breadcrumb = BOOT_BC_MAGIC | 0u;
    if (s_deadline) {
        esp_timer_stop(s_deadline);
    }
}

void clear()
{
    g_boot_breadcrumb = BOOT_BC_MAGIC | 0u;
}

}  // namespace boot_guard

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

    boot_guard::init();
    handle_pending_erase();
#if CONFIG_NCP_XIAO_EXT_ANTENNA
    select_external_antenna();
#else
    rf_switch_default_input();
#endif
    ESP_ERROR_CHECK(app::init());
    ESP_ERROR_CHECK(app::start());
}
