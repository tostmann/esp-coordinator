// Issue #17 diagnostics (CONFIG_NCP_DEBUG_USJ_STATUS): the USB-Serial/JTAG link
// goes silent after a host reboot while the chip still drains the port. From the
// host side "USJ IN path stuck, app alive" and "app event pump stalled, ISR alive"
// look identical, so this module watches both from an esp_timer callback (the
// esp_timer task runs above the ZBOSS, transport and main tasks) and reports on
// the UART0 console only — the USB-Serial/JTAG link itself stays log-free.
//
//  - every 50 ms: USB bus resets, bus active/idle (SOF progress), DTR/RTS
//    changes, routing flips, IN-token polling by the host, and two stall
//    detectors: TX pending (driver has IN_EMPTY enabled) for >3 s while the host
//    polls IN, and an app event pump that stopped iterating for >5 s (dumps the
//    FreeRTOS task list once).
//  - every 2 s: one status line with the raw controller registers, the
//    transport/app counters and the state of the relevant tasks.
//
// Register bits used here are read-only snapshots, except INT_CLR for the two
// raw bits this module consumes (USB_BUS_RESET, IN_TOKEN_REC_IN_EP1); the IDF
// driver neither enables nor clears those two.
#include "diag_usj.h"

#if CONFIG_NCP_DEBUG_USJ_STATUS

#include <atomic>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "hal/efuse_hal.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "boot_guard.h"
#include "version.h"

namespace diag {
namespace {

const char* TAG = "USJDIAG";

std::atomic<uint32_t> s_usb_rx_bytes{0};
std::atomic<uint32_t> s_usb_rx_reads{0};
std::atomic<uint32_t> s_usb_w_ok{0};
std::atomic<uint32_t> s_usb_w_fail{0};
std::atomic<uint32_t> s_route{0};
std::atomic<uint32_t> s_route_flips{0};
std::atomic<uint32_t> s_app_loops{0};
std::atomic<uint32_t> s_ev_in{0};
std::atomic<uint32_t> s_ev_out{0};
std::atomic<uint32_t> s_ev_other{0};

// Poll state — touched only from the esp_timer task.
uint32_t s_bus_resets = 0;
uint32_t s_last_fram = UINT32_MAX;
bool     s_bus_active = false;
uint32_t s_ctl_last = UINT32_MAX;
uint32_t s_last_route = 0;
uint32_t s_inpoll_interval = 0;       // 50 ms polls with an IN token since the last status line
int64_t  s_tx_pending_since = -1;     // us; -1 = driver has nothing to send
uint32_t s_tx_pending_inpolls = 0;    // IN-token polls during the current pending period
bool     s_tx_warned = false;
uint32_t s_last_loops = 0;
int64_t  s_loops_changed_at = 0;
bool     s_app_warned = false;

TaskStatus_t s_tasks[24];
char s_tasklist[1536];

const char* route_name(uint32_t r) { return r == 1 ? "USB" : r == 2 ? "UART" : "none"; }

char state_char(eTaskState st) {
    switch (st) {
        case eRunning:   return 'X';
        case eReady:     return 'R';
        case eBlocked:   return 'B';
        case eSuspended: return 'S';
        case eDeleted:   return 'D';
        default:         return '?';
    }
}

// "main:B zb:B trn:B IDLE:R" — the tasks that decide between the two hypotheses.
void task_states(char* out, size_t len) {
    out[0] = 0;
    UBaseType_t n = uxTaskGetSystemState(s_tasks, sizeof(s_tasks) / sizeof(s_tasks[0]), nullptr);
    size_t used = 0;
    for (UBaseType_t i = 0; i < n && used < len; ++i) {
        const char* nm = s_tasks[i].pcTaskName;
        const char* label = nullptr;
        if (!strcmp(nm, "main")) label = "main";
        else if (!strcmp(nm, "ncp_zb_task")) label = "zb";
        else if (!strcmp(nm, "transport")) label = "trn";
        else if (!strncmp(nm, "IDLE", 4)) label = nm;
        if (!label) continue;
        int w = snprintf(out + used, len - used, "%s%s:%c", used ? " " : "", label,
                         state_char(s_tasks[i].eCurrentState));
        if (w < 0) break;
        used += (size_t)w;
    }
}

void dump_tasks(const char* why) {
    vTaskList(s_tasklist);
    ESP_LOGW(TAG, "%s — task list (name state prio stack-hwm num):\n%s", why, s_tasklist);
}

void fast_cb(void*) {
    const int64_t now = esp_timer_get_time();
    const uint32_t ms = (uint32_t)(now / 1000);

    const uint32_t raw = REG_READ(USB_SERIAL_JTAG_INT_RAW_REG);
    uint32_t clr = 0;
    if (raw & USB_SERIAL_JTAG_USB_BUS_RESET_INT_RAW) {
        ++s_bus_resets;
        clr |= USB_SERIAL_JTAG_USB_BUS_RESET_INT_CLR;
        ESP_LOGI(TAG, "EV %lu ms: USB bus reset #%lu", (unsigned long)ms, (unsigned long)s_bus_resets);
    }
    const bool in_tok = raw & USB_SERIAL_JTAG_IN_TOKEN_REC_IN_EP1_INT_RAW;
    if (in_tok) {
        clr |= USB_SERIAL_JTAG_IN_TOKEN_REC_IN_EP1_INT_CLR;
        ++s_inpoll_interval;
    }
    if (clr) {
        REG_WRITE(USB_SERIAL_JTAG_INT_CLR_REG, clr);
    }

    // Bus activity: the SOF frame index advances every 1 ms while a host drives the bus.
    const uint32_t fram = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX_V;
    if (s_last_fram != UINT32_MAX) {
        const bool active = fram != s_last_fram;
        if (active != s_bus_active) {
            s_bus_active = active;
            ESP_LOGI(TAG, "EV %lu ms: bus %s (SOF %lu)", (unsigned long)ms, active ? "ACTIVE" : "IDLE",
                     (unsigned long)fram);
        }
    }
    s_last_fram = fram;

    // Host control lines as last set by SET_CONTROL_LINE_STATE.
    const uint32_t ctl = REG_READ(USB_SERIAL_JTAG_CHIP_RST_REG) & (USB_SERIAL_JTAG_RTS | USB_SERIAL_JTAG_DTR);
    if (ctl != s_ctl_last) {
        s_ctl_last = ctl;
        ESP_LOGI(TAG, "EV %lu ms: host lines DTR=%d RTS=%d", (unsigned long)ms,
                 (ctl & USB_SERIAL_JTAG_DTR) ? 1 : 0, (ctl & USB_SERIAL_JTAG_RTS) ? 1 : 0);
    }

    const uint32_t route = s_route.load(std::memory_order_relaxed);
    if (route != s_last_route) {
        s_last_route = route;
        ESP_LOGI(TAG, "EV %lu ms: TX route -> %s", (unsigned long)ms, route_name(route));
    }

    // TX stall: the IDF driver keeps SERIAL_IN_EMPTY enabled exactly while it has
    // bytes to send (ring/stash non-empty). Pending for >3 s is normal while the
    // host does not poll IN (port closed); pending while the host DOES poll means
    // nothing drains although the host is asking for data.
    const bool pending = REG_READ(USB_SERIAL_JTAG_INT_ENA_REG) & USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT_ENA;
    if (pending) {
        if (s_tx_pending_since < 0) {
            s_tx_pending_since = now;
            s_tx_pending_inpolls = 0;
        }
        if (in_tok) {
            ++s_tx_pending_inpolls;
        }
        if (!s_tx_warned && now - s_tx_pending_since > 3000000) {
            s_tx_warned = true;
            if (s_tx_pending_inpolls > 5) {
                ESP_LOGW(TAG, "EV %lu ms: TX STALL? driver has data pending for 3 s while the host polled IN "
                              "%lu times | ep1=%lx in1=%08lx raw=%04lx", (unsigned long)ms,
                         (unsigned long)s_tx_pending_inpolls,
                         (unsigned long)REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG),
                         (unsigned long)REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG), (unsigned long)raw);
            } else {
                ESP_LOGI(TAG, "EV %lu ms: TX pending for 3 s, host not polling IN (port closed?)",
                         (unsigned long)ms);
            }
        }
    } else if (s_tx_pending_since >= 0) {
        if (s_tx_warned) {
            ESP_LOGI(TAG, "EV %lu ms: TX drained after %lu ms", (unsigned long)ms,
                     (unsigned long)((now - s_tx_pending_since) / 1000));
        }
        s_tx_pending_since = -1;
        s_tx_warned = false;
    }

    // App event pump: iterates at least every 2 s (bounded xQueueReceive).
    const uint32_t loops = s_app_loops.load(std::memory_order_relaxed);
    if (loops != s_last_loops) {
        s_last_loops = loops;
        s_loops_changed_at = now;
        if (s_app_warned) {
            s_app_warned = false;
            ESP_LOGW(TAG, "EV %lu ms: app event pump running again", (unsigned long)ms);
        }
    } else if (!s_app_warned && loops != 0 && now - s_loops_changed_at > 5000000) {
        s_app_warned = true;
        ESP_LOGW(TAG, "EV %lu ms: APP PUMP STALL? no event-pump iteration for 5 s", (unsigned long)ms);
        dump_tasks("app pump stall");
    }
}

void status_cb(void*) {
    const int64_t now = esp_timer_get_time();
    char tasks[96];
    task_states(tasks, sizeof(tasks));
    const uint32_t ctl = REG_READ(USB_SERIAL_JTAG_CHIP_RST_REG);
    ESP_LOGI(TAG,
             "ST up=%lu.%01lus sof=%lu %s busrst=%lu inpoll=%lu dtr/rts=%d/%d | ena=%03lx raw=%04lx ep1=%lx "
             "in1=%08lx out1=%08lx | usb rx=%lu/%luB w=%lu/%lu route=%s flips=%lu | app loop=%lu ev in/out=%lu/%lu | %s",
             (unsigned long)(now / 1000000), (unsigned long)((now / 100000) % 10),
             (unsigned long)(REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX_V),
             s_bus_active ? "act" : "idle", (unsigned long)s_bus_resets, (unsigned long)s_inpoll_interval,
             (ctl & USB_SERIAL_JTAG_DTR) ? 1 : 0, (ctl & USB_SERIAL_JTAG_RTS) ? 1 : 0,
             (unsigned long)REG_READ(USB_SERIAL_JTAG_INT_ENA_REG), (unsigned long)REG_READ(USB_SERIAL_JTAG_INT_RAW_REG),
             (unsigned long)REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG), (unsigned long)REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG),
             (unsigned long)REG_READ(USB_SERIAL_JTAG_OUT_EP1_ST_REG),
             (unsigned long)s_usb_rx_reads.load(), (unsigned long)s_usb_rx_bytes.load(),
             (unsigned long)s_usb_w_ok.load(), (unsigned long)s_usb_w_fail.load(),
             route_name(s_route.load()), (unsigned long)s_route_flips.load(),
             (unsigned long)s_app_loops.load(), (unsigned long)s_ev_in.load(), (unsigned long)s_ev_out.load(),
             tasks);
    s_inpoll_interval = 0;
}

}  // namespace

void count_usb_rx(int bytes) {
    s_usb_rx_reads.fetch_add(1, std::memory_order_relaxed);
    s_usb_rx_bytes.fetch_add((uint32_t)bytes, std::memory_order_relaxed);
}
void count_usb_write(bool ok) {
    (ok ? s_usb_w_ok : s_usb_w_fail).fetch_add(1, std::memory_order_relaxed);
}
void count_route(uint8_t iface) {
    s_route.store(iface, std::memory_order_relaxed);
    s_route_flips.fetch_add(1, std::memory_order_relaxed);
}
void count_app_loop() {
    s_app_loops.fetch_add(1, std::memory_order_relaxed);
}
void count_app_event(int event) {
    // app::event_t: 0 = EVENT_INPUT (NCP->host), 1 = EVENT_OUTPUT (host->NCP), 2 = EVENT_RESET
    (event == 0 ? s_ev_in : event == 1 ? s_ev_out : s_ev_other).fetch_add(1, std::memory_order_relaxed);
}

void start() {
    // Console on UART0 through the driver, so log lines are buffered instead of
    // busy-waiting on the FIFO from the esp_timer task.
    if (uart_driver_install(UART_NUM_0, 256, 4096, 0, nullptr, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(UART_NUM_0);
    }
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    const int64_t now = esp_timer_get_time();
    const uint32_t rev = efuse_hal_chip_revision();
    ESP_LOGI(TAG, "esp-coordinator %s (built %s) DEBUG BUILD issue #17: USB-only, diagnostics on UART0",
             FW_VERSION_STRING, FW_BUILD_DATE);
    ESP_LOGI(TAG, "chip rev v%lu.%lu | reset reason esp=%d rom=0x%02x | safe_mode=%d | diag start at %lu ms",
             (unsigned long)(rev / 100), (unsigned long)(rev % 100), (int)esp_reset_reason(),
             (unsigned)esp_rom_get_reset_reason(0), boot_guard::safe_mode() ? 1 : 0,
             (unsigned long)(now / 1000));
    // Raw bits are NOT cleared by the 0x15/0x0C/0x03 resets (bench 2026-10-09), so this
    // snapshot can still carry events from before this boot.
    ESP_LOGI(TAG, "USJ at boot: raw=%04lx ena=%03lx ep1=%lx in1=%08lx out1=%08lx sof=%lu lines=%lx linecode=%08lx",
             (unsigned long)REG_READ(USB_SERIAL_JTAG_INT_RAW_REG), (unsigned long)REG_READ(USB_SERIAL_JTAG_INT_ENA_REG),
             (unsigned long)REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG), (unsigned long)REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG),
             (unsigned long)REG_READ(USB_SERIAL_JTAG_OUT_EP1_ST_REG),
             (unsigned long)(REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX_V),
             (unsigned long)REG_READ(USB_SERIAL_JTAG_CHIP_RST_REG),
             (unsigned long)REG_READ(USB_SERIAL_JTAG_SET_LINE_CODE_W0_REG));
    REG_WRITE(USB_SERIAL_JTAG_INT_CLR_REG,
              USB_SERIAL_JTAG_USB_BUS_RESET_INT_CLR | USB_SERIAL_JTAG_IN_TOKEN_REC_IN_EP1_INT_CLR);

    static esp_timer_handle_t s_fast = nullptr;
    static esp_timer_handle_t s_status = nullptr;
    const esp_timer_create_args_t fast_args = {
        .callback = fast_cb, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
        .name = "usjdiag_fast", .skip_unhandled_events = true,
    };
    const esp_timer_create_args_t status_args = {
        .callback = status_cb, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
        .name = "usjdiag_st", .skip_unhandled_events = true,
    };
    if (esp_timer_create(&fast_args, &s_fast) == ESP_OK) {
        esp_timer_start_periodic(s_fast, 50 * 1000);
    }
    if (esp_timer_create(&status_args, &s_status) == ESP_OK) {
        esp_timer_start_periodic(s_status, 2000 * 1000);
    }
}

}  // namespace diag

#endif  // CONFIG_NCP_DEBUG_USJ_STATUS
