#pragma once
#include <cstdint>

// WEDGE-1 boot guard: forensics + escape hatch for the "mute stick" failure
// class (observed 2026-06-06: a host power event left ESP32-C6 sticks with a
// permanently silent app — no boot frames, no ACKs, only a reflash appeared
// to heal them; leading theory is a hang/crash-loop early in ZBOSS init,
// e.g. mid NVRAM page migration, which also explains a later boot resuming
// a weeks-old dataset generation).
//
// Mechanism:
//  - An RTC-noinit breadcrumb counts boots that died before the ZBOSS stack
//    reached SKIP_STARTUP (continue_zboss). The counter survives esp_restart
//    and watchdog resets; true power-on garbage fails the magic check and
//    counts as a fresh start.
//  - A 30 s boot deadline (esp_timer) converts a silent early hang into a
//    reboot, so the counter can actually accumulate.
//  - After 3 consecutive early-boot failures the next boot enters SAFE MODE:
//    transport/protocol/app come up, the ZBOSS stack is NOT started, and the
//    command dispatch serves only ZBOSS-free commands (GET_MODULE_VERSION,
//    NCP_RESET) while answering everything else with GENERIC_BLOCKED — the
//    stick is host-visible and recoverable (NCP_RESET options=2 = factory
//    erase via the boot-time pending-erase path) instead of mute forever.
//    A safe-mode boot resets the counter, so the following reboot attempts
//    a normal start again.
namespace boot_guard {

// Evaluate the breadcrumb, bump the failure count, arm the boot deadline.
// Must be the first call in app_main (before any subsystem can hang).
void init();

// True when this boot runs in safe mode: the ZBOSS stack must not be
// started and dispatch is restricted (see zb_ncp::on_rx_data).
bool safe_mode();

// Called from zb_ncp::continue_zboss (ZBOSS reached SKIP_STARTUP, NVRAM
// dataset loaded): clears the failure count and cancels the boot deadline.
void mark_zboss_alive();

// Explicitly clear the failure count (host-supervised exit from safe mode).
// Called from the NCP_RESET path before the restart.
void clear();

// Stand the guard down for a boot that legitimately never starts ZBOSS:
// clears the failure count AND stops the 30 s deadline. Used by the wifi-coex
// Mode A (Improv-Serial provisioning) path — it runs no ZBOSS stack, so it
// never reaches mark_zboss_alive(); without this the deadline would reboot the
// provisioning session and the assume-fail breadcrumb would latch the device
// into safe mode.
void cancel();

}  // namespace boot_guard
