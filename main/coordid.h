#pragma once
//
// coordid — persisted "this coordinator had a live network" marker (Feature 1,
// silent-wipe detection). Stored in a dedicated NVS namespace "coordid" in the
// existing `nvs` partition (no partition-table change).
//
// On boot, if the ZBOSS stack comes up FACTORY-BLANK
// (ZB_BDB_SIGNAL_DEVICE_FIRST_START) while this marker says we WERE formed, the
// NVRAM was silently wiped or a poison backup was restored -> g_network_lost is
// raised so the condition can be surfaced (HTTP banner / log) instead of the
// coordinator silently sitting empty.
//
// Bench-proven failure class 2026-06-19 (Discussion #1 / peca89): a corrupt
// zb_storage page makes ZBOSS discard the dataset and reform blank, reaching
// SKIP_STARTUP normally (so the WEDGE-1 boot-guard sees a clean boot) -> the
// user's network vanishes with no signal at all.
//
// The marker is WRITTEN only from the ZBOSS-lock-safe signal context
// (DEVICE_REBOOT / FORMATION success); the wipe DETECTION reads it (NVS only,
// no zb_*) from the same context. nvs_flash_init() is already done by
// app::init() before any of these run.
//
#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Persisted identity marker. 'formed' gates the wipe alarm; pan/extpan/channel
// are kept for the human-facing "lost network was PAN 0x.." display only.
struct coord_identity_t {
    uint8_t  formed;      // 1 = this coordinator had a live network
    uint16_t pan;
    uint8_t  extpan[8];   // native order (zb_get_extended_pan_id fill order)
    uint8_t  channel;
    uint8_t  ver;         // struct version (= 1)
};

// Persist the marker. Compare-before-write: a no-op when the stored marker is
// identical, so a clean resume on every boot does NOT wear the flash.
esp_err_t coordid_save(const coord_identity_t *in);
// Load the marker; true iff a blob of the exact expected size exists. A size
// mismatch (struct grew) returns false = treated as "no marker" = no false
// wipe alarm (the safe failure mode).
bool      coordid_load(coord_identity_t *out);
// Remove the marker (an intentional factory/NVRAM reset is NOT a wipe).
// ESP_OK when the namespace/key never existed.
esp_err_t coordid_clear(void);

// Raised when a factory-blank boot is detected despite a "formed" marker.
// Written ONLY from the ZBOSS task; read lock-free from the web_info httpd task
// and (optionally) the NCP dispatch. A single volatile bool is a one-instruction
// load/store on the single-core C6 -> no torn read, and the reader never calls
// zb_*, so the big-lock rule is never touched on the read side.
extern volatile bool g_network_lost;

#ifdef __cplusplus
}
#endif
