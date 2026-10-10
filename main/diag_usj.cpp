// Issue #17 diagnostics (CONFIG_NCP_DEBUG_USJ_STATUS): the USB-Serial/JTAG link
// goes silent after a host reboot while the chip still drains the port. From the
// host side "USJ IN path stuck, app alive" and "app event pump stalled, ISR alive"
// look identical, so this module watches both from an esp_timer callback (the
// esp_timer task runs above the ZBOSS, transport and main tasks).
//
//  - every 50 ms: USB bus resets, bus active/idle (SOF progress), DTR/RTS
//    changes, routing flips, USB RX and write results, IN-token polling by the
//    host, the TX path (below), and an app event pump that stopped iterating for
//    >5 s (dumps the FreeRTOS task list once).
//  - every 2 s: one status line on the UART0 console with the raw controller
//    registers, the transport/app counters and the state of the relevant tasks.
//  - every event also goes to an event log in .noinit RAM: the last 4 boots,
//    128 records each, timestamps in ms of esp_timer time (from app start; the
//    systimer stops while a debugger halts the CPU). It survives the resets that
//    heal the silent link (USB RTS/DTR core reset 0x15, esp_restart 0x0C) and the
//    host reads it with DEBUG_USJ_LOG, so getting it needs no UART adapter.
//
// TX path: the IDF driver (5.5.3) enables SERIAL_IN_EMPTY while it has bytes
// queued and moves them into the IN FIFO only from that interrupt. The raw bit
// is an event the controller raises when the host has read a packet, and the
// driver relies on it being pending whenever it enables the interrupt. Data
// queued + IN FIFO free + no event pending therefore means nothing will ever be
// sent: the state a lost event leaves behind (bench 2026-10-09, it gives the
// field signature exactly). With CONFIG_NCP_DEBUG_USJ_TX_KICK a zero-length
// packet (WR_DONE on the empty FIFO) restarts it: once the host has read the
// packet, the controller raises the event and the driver carries on.
//
// Register bits used here are read-only snapshots, except INT_CLR for the two
// raw bits this module consumes (USB_BUS_RESET, IN_TOKEN_REC_IN_EP1; the IDF
// driver neither enables nor clears those two) and WR_DONE for the kick.
#include "diag_usj.h"

#if CONFIG_NCP_DEBUG_USJ_STATUS

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "hal/efuse_hal.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "boot_guard.h"
#include "version.h"

namespace diag {
namespace {

const char* TAG = "USJDIAG";

// ---------------------------------------------------------------------------
// Event log. Layout is read by test/issue17_log.py — keep the two in step.

constexpr uint32_t LOG_MAGIC  = 0x4C4A5355;  // "USJL"
constexpr uint32_t LOG_LAYOUT = 1;
constexpr uint32_t DIAG_REV   = 2;           // diagnostic build revision
constexpr int SLOTS = 4;                     // boots kept
constexpr int HEAD  = 48;                    // first records of a boot, never overwritten
constexpr int RING  = 80;                    // newest records after those

struct rec_t {
    uint32_t t_ms;      // esp_timer time of this boot
    uint8_t  type;
    uint8_t  a;
    uint16_t b;
    uint32_t c;
    uint32_t d;
};
static_assert(sizeof(rec_t) == 16, "rec_t layout");

struct slot_t {
    uint32_t boot_seq;  // 0 = unused
    uint32_t count;     // records added this boot
    uint32_t alive_ms;  // last 50 ms poll of this boot
    uint32_t sum;       // XOR over rec[] — shows whether RAM was kept across the reset
    rec_t    rec[HEAD + RING];
};

struct store_t {
    uint32_t magic;
    uint32_t layout;
    uint32_t boot_seq;
    uint32_t cur;
    uint32_t check;     // over the four words above
    uint32_t reserved[3];
    slot_t   slot[SLOTS];
};

// Readout header, sent in front of the store.
struct live_t {
    uint32_t magic;
    uint16_t layout;
    uint16_t header_size;
    uint32_t fw_version;  // MAJOR << 24 | MINOR << 16 | BUILD
    uint32_t diag_rev;
    uint32_t uptime_ms;
    uint32_t flags;       // FLAG_*
    uint8_t  cur, slots, head, ring;
    uint32_t int_raw, int_ena, ep1_conf, in_ep1_st, out_ep1_st, fram_num, chip_rst;
    uint32_t kicks;
    uint32_t usb_rx_bytes;
    uint32_t usb_w_ok;
    uint32_t usb_w_fail;
};
static_assert(sizeof(live_t) == 72, "live_t layout");
static_assert(sizeof(store_t) == 32 + SLOTS * (16 + (HEAD + RING) * 16), "store_t layout");

enum : uint32_t {
    FLAG_KICK     = 1,   // TX kick compiled in
    FLAG_UART     = 2,   // NCP UART transport compiled in
    FLAG_SAFE     = 4,   // boot guard safe mode
    FLAG_KEPT     = 8,   // log from earlier boots was kept at this boot
    FLAG_SNAPSHOT = 16,  // image is a consistent snapshot
};

enum : uint8_t {
    EV_BOOT = 1,    // a=esp_reset_reason b=ROM reset cause c=boot seq d=FLAG_* | chip rev << 16
    EV_REGS,        // a=REGS_* b=SOF frame c=INT_RAW | INT_ENA << 16 d=EP1_CONF | CHIP_RST << 4 | IN_EP1_ST << 16
    EV_BUS_RESET,   // b=count
    EV_BUS,         // a=1 active / 0 idle b=SOF frame
    EV_LINES,       // a=DTR b=RTS
    EV_ROUTE,       // a=0 none / 1 USB / 2 UART b=flips
    EV_RX,          // host bytes after boot or after >=2 s without: c=bytes total d=reads total
    EV_WRITE_FAIL,  // first USB write that did not fit, after a good one: c=ok total d=failed total
    EV_WRITE_OK,    // first good USB write after failures: c=ok total d=failed total
    EV_TX_WAIT,     // data queued for 3 s: a=TXW_* b=IN polls c=ms queued
    EV_TX_KICK,     // zero-length packet sent: a=kick no. b=IN polls c=ms queued d=ms without event
    EV_TX_LOST,     // lost event seen, kick not compiled in: b=IN polls c=ms queued d=ms without event
    EV_TX_DRAINED,  // queued data went out: a=kicks b=IN polls c=ms queued
    EV_APP_STALL,   // event pump did not iterate for 5 s: c=task states d=events in | out << 16
    EV_APP_RESUME,  // c=ms stalled
    EV_HEARTBEAT,   // every 30 s (idle: 5 min): a=HB_* b=IN polls c=USB RX bytes total d=USB writes ok total
    EV_READOUT,     // host read this log: c=task states (last status poll)
};
enum : uint8_t { REGS_BOOT = 0, REGS_TX_WAIT, REGS_KICK, REGS_LOST, REGS_READOUT };
enum : uint8_t { TXW_HOST_NOT_POLLING = 0, TXW_FIFO_BUSY = 1, TXW_EVENT_LOST = 2 };
enum : uint8_t { HB_BUS_ACTIVE = 1, HB_DTR = 2, HB_RTS = 4, HB_TX_QUEUED = 8 };

__NOINIT_ATTR store_t s_log;
portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_log_ready = false;
bool s_log_kept = false;
uint8_t* s_snap = nullptr;   // readout snapshot, app task only

uint32_t header_check() {
    return s_log.magic ^ s_log.layout ^ s_log.boot_seq ^ s_log.cur ^ 0x5A5AA5A5u;
}

void xor_rec(uint32_t& sum, const rec_t& r) {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(&r);
    sum ^= w[0] ^ w[1] ^ w[2] ^ w[3];
}

uint16_t sat16(uint32_t v) { return v > 0xFFFF ? 0xFFFF : (uint16_t)v; }
uint8_t sat8(uint32_t v) { return v > 0xFF ? 0xFF : (uint8_t)v; }
uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

void ev(uint8_t type, uint8_t a, uint16_t b, uint32_t c, uint32_t d) {
    if (!s_log_ready) {
        return;
    }
    const rec_t r = {now_ms(), type, a, b, c, d};
    portENTER_CRITICAL(&s_log_mux);
    slot_t& s = s_log.slot[s_log.cur];
    const uint32_t idx = s.count < HEAD ? s.count : HEAD + (s.count - HEAD) % RING;
    xor_rec(s.sum, s.rec[idx]);  // the record it replaces (all zero until the ring wraps)
    s.rec[idx] = r;
    xor_rec(s.sum, r);
    ++s.count;
    portEXIT_CRITICAL(&s_log_mux);
}

void ev_regs(uint8_t why, uint32_t raw, uint32_t ena, uint32_t ep1) {
    const uint32_t in1 = REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG);
    const uint32_t ctl = REG_READ(USB_SERIAL_JTAG_CHIP_RST_REG);
    const uint32_t fram = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX_V;
    ev(EV_REGS, why, (uint16_t)fram, (raw & 0xFFFF) | (ena & 0xFFFF) << 16,
       (ep1 & 0xF) | (ctl & 0xF) << 4 | (in1 & 0xFFFF) << 16);
}

void ev_regs_now(uint8_t why) {
    ev_regs(why, REG_READ(USB_SERIAL_JTAG_INT_RAW_REG), REG_READ(USB_SERIAL_JTAG_INT_ENA_REG),
            REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG));
}

void log_alive(uint32_t ms) {
    portENTER_CRITICAL(&s_log_mux);
    s_log.slot[s_log.cur].alive_ms = ms;
    portEXIT_CRITICAL(&s_log_mux);
}

// Keep the earlier boots if the header survived the reset, then open this
// boot's slot. A power-on normally leaves RAM garbage and fails the check;
// a slot that did not survive intact shows up as an XOR mismatch on the host.
void log_init() {
    s_log_kept = s_log.magic == LOG_MAGIC && s_log.layout == LOG_LAYOUT && s_log.cur < SLOTS &&
                 s_log.check == header_check();
    if (!s_log_kept) {
        memset(&s_log, 0, sizeof(s_log));
        s_log.magic = LOG_MAGIC;
        s_log.layout = LOG_LAYOUT;
        s_log.cur = SLOTS - 1;
    }
    s_log.cur = (s_log.cur + 1) % SLOTS;
    s_log.boot_seq += 1;
    memset(&s_log.slot[s_log.cur], 0, sizeof(slot_t));
    s_log.slot[s_log.cur].boot_seq = s_log.boot_seq;
    s_log.check = header_check();
    s_log_ready = true;
}

uint32_t build_flags() {
    uint32_t f = 0;
#if CONFIG_NCP_DEBUG_USJ_TX_KICK
    f |= FLAG_KICK;
#endif
#if CONFIG_NCP_UART_TRANSPORT
    f |= FLAG_UART;
#endif
    if (boot_guard::safe_mode()) f |= FLAG_SAFE;
    if (s_log_kept) f |= FLAG_KEPT;
    return f;
}

// ---------------------------------------------------------------------------
// Counters fed by the transport/app hooks.

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
std::atomic<uint32_t> s_kicks{0};

// Poll state — touched only from the esp_timer task.
uint32_t s_bus_resets = 0;
uint32_t s_last_fram = UINT32_MAX;
bool     s_bus_active = false;
uint32_t s_ctl_last = UINT32_MAX;
uint32_t s_last_route = 0;
uint32_t s_inpoll_interval = 0;   // 50 ms polls with an IN token since the last status line
uint32_t s_hb_inpolls = 0;        // the same since the last heartbeat
uint32_t s_hb_div = 0;
uint32_t s_hb_quiet = 0;          // heartbeats skipped because nothing changed
uint32_t s_hb_last_rx = 0;
uint32_t s_hb_last_w = 0;
uint8_t  s_hb_last_flags = 0xFF;
uint32_t s_rx_seen = 0;
int64_t  s_rx_last_at = -1;
uint32_t s_w_ok_seen = 0;
uint32_t s_w_fail_seen = 0;
bool     s_w_failing = false;
int64_t  s_tx_since = -1;         // us; -1 = driver has nothing queued
uint32_t s_tx_inpolls = 0;        // IN-token polls while queued
bool     s_tx_waited = false;     // EV_TX_WAIT sent for this period
int64_t  s_lost_since = -1;       // us; -1 = not in the lost-event state
uint32_t s_period_kicks = 0;
bool     s_lost_reported = false;
uint32_t s_last_loops = 0;
int64_t  s_loops_changed_at = 0;
bool     s_app_warned = false;

TaskStatus_t s_tasks[24];
char s_tasklist[1536];

// esp_log (log v1) hands over one complete line per call. Through the UART VFS
// it would be pushed into the driver's NOSPLIT ring one byte per item and the
// caller would wait for room — several ms per status line, inside the
// esp_timer task that ZBOSS timers share. Here a line is written in one piece
// if it fits and dropped (counted) otherwise; nobody ever waits.
SemaphoreHandle_t s_out_mux = nullptr;
char s_out_buf[512];
std::atomic<uint32_t> s_out_dropped{0};

int log_vprintf(const char* fmt, va_list ap) {
    if (xPortInIsrContext() || xSemaphoreTake(s_out_mux, 0) != pdTRUE) {
        s_out_dropped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const int n = vsnprintf(s_out_buf, sizeof(s_out_buf), fmt, ap);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(s_out_buf)) {
            len = sizeof(s_out_buf) - 1;
            s_out_buf[len - 1] = '\n';
        }
        size_t room = 0;
        if (uart_get_tx_buffer_free_size(UART_NUM_0, &room) == ESP_OK && room >= len + 32) {
            uart_write_bytes(UART_NUM_0, s_out_buf, len);
        } else {
            s_out_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    xSemaphoreGive(s_out_mux);
    return n;
}

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

// State letters of the tasks that decide between the two hypotheses, one byte
// each: main (event pump), ncp_zb_task, transport, IDLE ('-' = not found).
// esp_timer task only — s_tasks is not shared.
std::atomic<uint32_t> s_task_states{0x2D2D2D2Du};

uint32_t poll_task_states() {
    char st[4] = {'-', '-', '-', '-'};
    const UBaseType_t n = uxTaskGetSystemState(s_tasks, sizeof(s_tasks) / sizeof(s_tasks[0]), nullptr);
    for (UBaseType_t i = 0; i < n; ++i) {
        const char* nm = s_tasks[i].pcTaskName;
        const int k = !strcmp(nm, "main") ? 0 : !strcmp(nm, "ncp_zb_task") ? 1 : !strcmp(nm, "transport") ? 2
                    : !strncmp(nm, "IDLE", 4) ? 3 : -1;
        if (k >= 0) {
            st[k] = state_char(s_tasks[i].eCurrentState);
        }
    }
    const uint32_t packed = (uint32_t)(uint8_t)st[0] | (uint32_t)(uint8_t)st[1] << 8 |
                            (uint32_t)(uint8_t)st[2] << 16 | (uint32_t)(uint8_t)st[3] << 24;
    s_task_states.store(packed, std::memory_order_relaxed);
    return packed;
}

void dump_tasks(const char* why) {
    vTaskList(s_tasklist);
    ESP_LOGW(TAG, "%s — task list (name state prio stack-hwm num):", why);
    char* save = nullptr;
    for (char* line = strtok_r(s_tasklist, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        ESP_LOGW(TAG, "  %s", line);
    }
}

#if CONFIG_NCP_DEBUG_USJ_TX_KICK
portMUX_TYPE s_kick_mux = portMUX_INITIALIZER_UNLOCKED;

// Zero-length packet: WR_DONE on the empty IN FIFO. Checked and written with
// interrupts off, so the driver ISR cannot put data into the FIFO in between;
// the state cannot change meanwhile either (the event needs a packet the host
// has read, and the FIFO is empty).
bool kick_zlp() {
    bool sent = false;
    portENTER_CRITICAL(&s_kick_mux);
    if ((REG_READ(USB_SERIAL_JTAG_INT_ENA_REG) & USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT_ENA) &&
        !(REG_READ(USB_SERIAL_JTAG_INT_RAW_REG) & USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT_RAW) &&
        usb_serial_jtag_ll_txfifo_writable()) {
        usb_serial_jtag_ll_txfifo_flush();
        sent = true;
    }
    portEXIT_CRITICAL(&s_kick_mux);
    return sent;
}
#endif

void tx_poll(int64_t now, uint32_t ms, uint32_t raw, bool in_tok) {
    const uint32_t ena = REG_READ(USB_SERIAL_JTAG_INT_ENA_REG);
    const uint32_t ep1 = REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG);
    if (!(ena & USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT_ENA)) {
        if (s_tx_since >= 0) {
            const uint32_t q_ms = (uint32_t)((now - s_tx_since) / 1000);
            if (s_tx_waited || s_period_kicks || s_lost_reported) {
                ev(EV_TX_DRAINED, sat8(s_period_kicks), sat16(s_tx_inpolls), q_ms, 0);
                ESP_LOGI(TAG, "EV %lu ms: TX drained after %lu ms (%lu kicks)", (unsigned long)ms,
                         (unsigned long)q_ms, (unsigned long)s_period_kicks);
            }
            s_tx_since = -1;
            s_lost_since = -1;
            s_tx_waited = false;
            s_period_kicks = 0;
            s_lost_reported = false;
        }
        return;
    }
    if (s_tx_since < 0) {
        s_tx_since = now;
        s_tx_inpolls = 0;
    }
    if (in_tok) {
        ++s_tx_inpolls;
    }
    const uint32_t q_ms = (uint32_t)((now - s_tx_since) / 1000);

    const bool lost = (ep1 & USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE) &&
                      !(raw & USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT_RAW);
    if (!lost) {
        s_lost_since = -1;
    } else {
        if (s_lost_since < 0) {
            s_lost_since = now;
        }
        const uint32_t lost_ms = (uint32_t)((now - s_lost_since) / 1000);
#if CONFIG_NCP_DEBUG_USJ_TX_KICK
        // 1 s apart for the first three kicks of a period, then every 10 s.
        if (lost_ms >= (s_period_kicks < 3 ? 1000u : 10000u)) {
            const uint32_t raw_before = REG_READ(USB_SERIAL_JTAG_INT_RAW_REG);
            if (kick_zlp()) {
                const uint32_t n = s_kicks.fetch_add(1) + 1;
                ++s_period_kicks;
                ev_regs(REGS_KICK, raw_before, ena, ep1);
                ev(EV_TX_KICK, sat8(n), sat16(s_tx_inpolls), q_ms, lost_ms);
                if (n <= 20 || n % 100 == 0) {
                    ESP_LOGW(TAG, "EV %lu ms: TX KICK #%lu: data queued %lu ms, IN FIFO free and no IN_EMPTY event "
                                  "for %lu ms -> zero-length packet (host IN polls %lu)",
                             (unsigned long)ms, (unsigned long)n, (unsigned long)q_ms, (unsigned long)lost_ms,
                             (unsigned long)s_tx_inpolls);
                }
            }
            s_lost_since = now;
        }
#else
        if (!s_lost_reported && lost_ms >= 1000) {
            s_lost_reported = true;
            ev_regs(REGS_LOST, raw, ena, ep1);
            ev(EV_TX_LOST, 0, sat16(s_tx_inpolls), q_ms, lost_ms);
            ESP_LOGW(TAG, "EV %lu ms: TX STALL: data queued, IN FIFO free and no IN_EMPTY event for 1 s "
                          "(lost event; kick not compiled in)", (unsigned long)ms);
        }
#endif
    }

    if (!s_tx_waited && q_ms > 3000) {
        s_tx_waited = true;
        const uint8_t st = lost ? TXW_EVENT_LOST : s_tx_inpolls > 5 ? TXW_FIFO_BUSY : TXW_HOST_NOT_POLLING;
        ev_regs(REGS_TX_WAIT, raw, ena, ep1);
        ev(EV_TX_WAIT, st, sat16(s_tx_inpolls), q_ms, 0);
        if (st == TXW_HOST_NOT_POLLING) {
            ESP_LOGI(TAG, "EV %lu ms: TX queued for 3 s, host not polling IN (port closed?)", (unsigned long)ms);
        } else {
            ESP_LOGW(TAG, "EV %lu ms: TX STALL? queued for 3 s, %s, host polled IN %lu times | ep1=%lx in1=%08lx raw=%04lx",
                     (unsigned long)ms, st == TXW_EVENT_LOST ? "IN FIFO free, no IN_EMPTY event" : "IN FIFO busy",
                     (unsigned long)s_tx_inpolls, (unsigned long)ep1,
                     (unsigned long)REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG), (unsigned long)raw);
        }
    }
}

void fast_cb(void*) {
    const int64_t now = esp_timer_get_time();
    const uint32_t ms = (uint32_t)(now / 1000);
    log_alive(ms);

    const uint32_t raw = REG_READ(USB_SERIAL_JTAG_INT_RAW_REG);
    uint32_t clr = 0;
    if (raw & USB_SERIAL_JTAG_USB_BUS_RESET_INT_RAW) {
        ++s_bus_resets;
        clr |= USB_SERIAL_JTAG_USB_BUS_RESET_INT_CLR;
        ev(EV_BUS_RESET, 0, sat16(s_bus_resets), 0, 0);
        ESP_LOGI(TAG, "EV %lu ms: USB bus reset #%lu", (unsigned long)ms, (unsigned long)s_bus_resets);
    }
    const bool in_tok = raw & USB_SERIAL_JTAG_IN_TOKEN_REC_IN_EP1_INT_RAW;
    if (in_tok) {
        clr |= USB_SERIAL_JTAG_IN_TOKEN_REC_IN_EP1_INT_CLR;
        ++s_inpoll_interval;
        ++s_hb_inpolls;
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
            ev(EV_BUS, active ? 1 : 0, (uint16_t)fram, 0, 0);
            ESP_LOGI(TAG, "EV %lu ms: bus %s (SOF %lu)", (unsigned long)ms, active ? "ACTIVE" : "IDLE",
                     (unsigned long)fram);
        }
    }
    s_last_fram = fram;

    // Host control lines as last set by SET_CONTROL_LINE_STATE.
    const uint32_t ctl = REG_READ(USB_SERIAL_JTAG_CHIP_RST_REG) & (USB_SERIAL_JTAG_RTS | USB_SERIAL_JTAG_DTR);
    if (ctl != s_ctl_last) {
        s_ctl_last = ctl;
        const int dtr = (ctl & USB_SERIAL_JTAG_DTR) ? 1 : 0;
        const int rts = (ctl & USB_SERIAL_JTAG_RTS) ? 1 : 0;
        ev(EV_LINES, (uint8_t)dtr, (uint16_t)rts, 0, 0);
        ESP_LOGI(TAG, "EV %lu ms: host lines DTR=%d RTS=%d", (unsigned long)ms, dtr, rts);
    }

    const uint32_t route = s_route.load(std::memory_order_relaxed);
    if (route != s_last_route) {
        s_last_route = route;
        ev(EV_ROUTE, (uint8_t)route, sat16(s_route_flips.load()), 0, 0);
        ESP_LOGI(TAG, "EV %lu ms: TX route -> %s", (unsigned long)ms, route_name(route));
    }

    // Host -> NCP bytes: log the start of each burst (first bytes, or after 2 s without).
    const uint32_t rx = s_usb_rx_bytes.load(std::memory_order_relaxed);
    if (rx != s_rx_seen) {
        if (s_rx_last_at < 0 || now - s_rx_last_at >= 2000000) {
            ev(EV_RX, 0, 0, rx, s_usb_rx_reads.load());
            ESP_LOGI(TAG, "EV %lu ms: USB RX from host (total %lu B)", (unsigned long)ms, (unsigned long)rx);
        }
        s_rx_seen = rx;
        s_rx_last_at = now;
    }

    // NCP -> host writes: a failed write means the 2 KB TX ring stayed full.
    const uint32_t wo = s_usb_w_ok.load(std::memory_order_relaxed);
    const uint32_t wf = s_usb_w_fail.load(std::memory_order_relaxed);
    if (wf != s_w_fail_seen && !s_w_failing) {
        s_w_failing = true;
        ev(EV_WRITE_FAIL, 0, 0, wo, wf);
        ESP_LOGW(TAG, "EV %lu ms: USB write did not fit (TX ring full), ok=%lu failed=%lu", (unsigned long)ms,
                 (unsigned long)wo, (unsigned long)wf);
    } else if (wo != s_w_ok_seen && wf == s_w_fail_seen && s_w_failing) {
        s_w_failing = false;
        ev(EV_WRITE_OK, 0, 0, wo, wf);
        ESP_LOGI(TAG, "EV %lu ms: USB writes fit again", (unsigned long)ms);
    }
    s_w_ok_seen = wo;
    s_w_fail_seen = wf;

    tx_poll(now, ms, raw, in_tok);

    // App event pump: iterates at least every 2 s (bounded xQueueReceive).
    const uint32_t loops = s_app_loops.load(std::memory_order_relaxed);
    if (loops != s_last_loops) {
        if (s_app_warned) {
            s_app_warned = false;
            ev(EV_APP_RESUME, 0, 0, (uint32_t)((now - s_loops_changed_at) / 1000), 0);
            ESP_LOGW(TAG, "EV %lu ms: app event pump running again", (unsigned long)ms);
        }
        s_last_loops = loops;
        s_loops_changed_at = now;
    } else if (!s_app_warned && loops != 0 && now - s_loops_changed_at > 5000000) {
        s_app_warned = true;
        ev(EV_APP_STALL, 0, 0, poll_task_states(),
           (s_ev_in.load() & 0xFFFF) | (s_ev_out.load() & 0xFFFF) << 16);
        ESP_LOGW(TAG, "EV %lu ms: APP PUMP STALL? no event-pump iteration for 5 s", (unsigned long)ms);
        dump_tasks("app pump stall");
    }
}

void status_cb(void*) {
    const int64_t now = esp_timer_get_time();
    const uint32_t ts = poll_task_states();
    const uint32_t ctl = REG_READ(USB_SERIAL_JTAG_CHIP_RST_REG);
    const uint32_t ena = REG_READ(USB_SERIAL_JTAG_INT_ENA_REG);
    ESP_LOGI(TAG,
             "ST up=%lu.%01lus sof=%lu %s busrst=%lu inpoll=%lu dtr/rts=%d/%d | ena=%03lx raw=%04lx ep1=%lx "
             "in1=%08lx out1=%08lx | usb rx=%lu/%luB w=%lu/%lu route=%s flips=%lu kicks=%lu | app loop=%lu "
             "ev in/out=%lu/%lu | main:%c zb:%c trn:%c IDLE:%c | log drops %lu",
             (unsigned long)(now / 1000000), (unsigned long)((now / 100000) % 10),
             (unsigned long)(REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX_V),
             s_bus_active ? "act" : "idle", (unsigned long)s_bus_resets, (unsigned long)s_inpoll_interval,
             (ctl & USB_SERIAL_JTAG_DTR) ? 1 : 0, (ctl & USB_SERIAL_JTAG_RTS) ? 1 : 0,
             (unsigned long)ena, (unsigned long)REG_READ(USB_SERIAL_JTAG_INT_RAW_REG),
             (unsigned long)REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG), (unsigned long)REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG),
             (unsigned long)REG_READ(USB_SERIAL_JTAG_OUT_EP1_ST_REG),
             (unsigned long)s_usb_rx_reads.load(), (unsigned long)s_usb_rx_bytes.load(),
             (unsigned long)s_usb_w_ok.load(), (unsigned long)s_usb_w_fail.load(),
             route_name(s_route.load()), (unsigned long)s_route_flips.load(), (unsigned long)s_kicks.load(),
             (unsigned long)s_app_loops.load(), (unsigned long)s_ev_in.load(), (unsigned long)s_ev_out.load(),
             (char)(ts & 0xFF), (char)(ts >> 8 & 0xFF), (char)(ts >> 16 & 0xFF), (char)(ts >> 24),
             (unsigned long)s_out_dropped.load());
    s_inpoll_interval = 0;

    // Heartbeat every 30 s, but an idle link only every 5 min so it does not
    // push the events out of the ring.
    if (++s_hb_div >= 15) {
        s_hb_div = 0;
        const uint8_t f = (s_bus_active ? HB_BUS_ACTIVE : 0) | ((ctl & USB_SERIAL_JTAG_DTR) ? HB_DTR : 0) |
                          ((ctl & USB_SERIAL_JTAG_RTS) ? HB_RTS : 0) |
                          ((ena & USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT_ENA) ? HB_TX_QUEUED : 0);
        const uint32_t rx = s_usb_rx_bytes.load();
        const uint32_t w = s_usb_w_ok.load();
        if (f != s_hb_last_flags || rx != s_hb_last_rx || w != s_hb_last_w || s_hb_inpolls || ++s_hb_quiet >= 10) {
            ev(EV_HEARTBEAT, f, sat16(s_hb_inpolls), rx, w);
            s_hb_inpolls = 0;
            s_hb_quiet = 0;
            s_hb_last_flags = f;
            s_hb_last_rx = rx;
            s_hb_last_w = w;
        }
    }
}

void fill_live(live_t& l, bool snapshot) {
    memset(&l, 0, sizeof(l));
    l.magic = LOG_MAGIC;
    l.layout = LOG_LAYOUT;
    l.header_size = sizeof(live_t);
    l.fw_version = (uint32_t)FW_VERSION_MAJOR << 24 | (uint32_t)FW_VERSION_MINOR << 16 | (FW_VERSION_BUILD & 0xFFFF);
    l.diag_rev = DIAG_REV;
    l.uptime_ms = now_ms();
    l.flags = build_flags() | (snapshot ? (uint32_t)FLAG_SNAPSHOT : 0u);
    l.cur = (uint8_t)s_log.cur;
    l.slots = SLOTS;
    l.head = HEAD;
    l.ring = RING;
    l.int_raw = REG_READ(USB_SERIAL_JTAG_INT_RAW_REG);
    l.int_ena = REG_READ(USB_SERIAL_JTAG_INT_ENA_REG);
    l.ep1_conf = REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG);
    l.in_ep1_st = REG_READ(USB_SERIAL_JTAG_IN_EP1_ST_REG);
    l.out_ep1_st = REG_READ(USB_SERIAL_JTAG_OUT_EP1_ST_REG);
    l.fram_num = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG);
    l.chip_rst = REG_READ(USB_SERIAL_JTAG_CHIP_RST_REG);
    l.kicks = s_kicks.load();
    l.usb_rx_bytes = s_usb_rx_bytes.load();
    l.usb_w_ok = s_usb_w_ok.load();
    l.usb_w_fail = s_usb_w_fail.load();
}

// The other slots are frozen; only this boot's slot needs the lock.
void copy_store(uint8_t* dst) {
    const size_t cur_off = offsetof(store_t, slot) + s_log.cur * sizeof(slot_t);
    memcpy(dst, &s_log, sizeof(store_t));
    portENTER_CRITICAL(&s_log_mux);
    memcpy(dst + cur_off, reinterpret_cast<const uint8_t*>(&s_log) + cur_off, sizeof(slot_t));
    portEXIT_CRITICAL(&s_log_mux);
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

size_t log_read(uint32_t offset, uint8_t* out, size_t len, uint32_t* total) {
    constexpr uint32_t size = sizeof(live_t) + sizeof(store_t);
    *total = size;
    if (!s_log_ready) {
        *total = 0;
        return 0;
    }
    if (offset == 0) {
        ev_regs_now(REGS_READOUT);
        ev(EV_READOUT, 0, 0, s_task_states.load(), 0);
        ESP_LOGI(TAG, "EV %lu ms: event log read by the host", (unsigned long)now_ms());
        if (!s_snap) {
            s_snap = static_cast<uint8_t*>(malloc(size));
        }
        if (s_snap) {
            fill_live(*reinterpret_cast<live_t*>(s_snap), true);
            copy_store(s_snap + sizeof(live_t));
        }
    }
    if (offset >= size) {
        return 0;
    }
    const size_t n = len < size - offset ? len : size - offset;
    if (s_snap) {
        memcpy(out, s_snap + offset, n);
        if (offset + n >= size) {
            free(s_snap);
            s_snap = nullptr;
        }
    } else {
        // No memory for a snapshot: serve the live data.
        live_t l;
        fill_live(l, false);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t at = offset + i;
            out[i] = at < sizeof(live_t) ? reinterpret_cast<const uint8_t*>(&l)[at]
                                         : reinterpret_cast<const uint8_t*>(&s_log)[at - sizeof(live_t)];
        }
    }
    return n;
}

void start() {
    // Console on UART0 through the driver, TX only (the NCP UART transport may
    // own GPIO23 = U0RXD on the H2). Log lines go through log_vprintf so the
    // esp_timer task never waits for the UART.
    if (uart_driver_install(UART_NUM_0, 256, 4096, 0, nullptr, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(UART_NUM_0);
        s_out_mux = xSemaphoreCreateMutex();
        if (s_out_mux) {
            esp_log_set_vprintf(log_vprintf);
        }
    }
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    log_init();
    const int rr = (int)esp_reset_reason();
    const unsigned rom = (unsigned)esp_rom_get_reset_reason(0);
    const uint32_t rev = efuse_hal_chip_revision();
    const uint32_t flags = build_flags();
    ev(EV_BOOT, (uint8_t)rr, (uint16_t)rom, s_log.boot_seq, flags | (rev & 0xFFFF) << 16);
    // Raw bits are NOT cleared by the 0x15/0x0C/0x03 resets (bench 2026-10-09), so this
    // snapshot can still carry events from before this boot.
    ev_regs_now(REGS_BOOT);

    ESP_LOGI(TAG, "esp-coordinator %s (built %s) DEBUG BUILD issue #17 r%lu: TX kick %s, NCP UART %s",
             FW_VERSION_STRING, FW_BUILD_DATE, (unsigned long)DIAG_REV, (flags & FLAG_KICK) ? "on" : "off",
             (flags & FLAG_UART) ? "on" : "off");
    ESP_LOGI(TAG, "chip rev v%lu.%lu | reset reason esp=%d rom=0x%02x | safe_mode=%d | boot #%lu, log %s | "
                  "diag start at %lu ms",
             (unsigned long)(rev / 100), (unsigned long)(rev % 100), rr, rom, boot_guard::safe_mode() ? 1 : 0,
             (unsigned long)s_log.boot_seq, s_log_kept ? "kept" : "new", (unsigned long)now_ms());
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
