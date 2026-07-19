#pragma once
//
// coord_health — live coordinator health cache for the Mode-B HTTP status page
// (Feature 2). WRITTEN only from the ZBOSS-lock-safe signal context (where the
// zb_* getters are legal); READ lock-free by the web_info httpd task, which
// must NEVER call any zb_* itself (big-lock rule -> the v1.3.49
// vPortExitCritical panic class).
//
// The single-word scalars are plain volatile (naturally atomic on the 32-bit
// single-core C6). The 8-byte extended PAN is published behind a seqlock
// (extpan_seq even=stable / odd=write-in-progress) so the reader never sees a
// torn half-old/half-new value.
//
// g_network_lost (the silent-wipe flag) is owned/declared by coordid.h; this
// module only consumes it for the banner.
//
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct coord_health {
    volatile bool     valid;        // true once the first ZBOSS publish happened
    volatile bool     joined;
    volatile uint16_t pan;          // 0xFFFF = not joined
    volatile uint8_t  channel;      // 0xFF = unknown
    volatile uint16_t lost_pan;     // last-known PAN at wipe detection (display)
    volatile uint8_t  lost_channel; // last-known channel at wipe detection
    volatile uint32_t extpan_seq;   // seqlock: even=stable, odd=write-in-progress
    uint8_t           extpan[8];    // native order
};
extern struct coord_health g_coord_health;

// Publish current identity. Pure data — the caller already read the zb_* getters
// in a lock-safe context and passes the values in. Call ONLY from the ZBOSS
// signal context.
void coord_health_publish(bool joined, uint16_t pan, uint8_t channel,
                          const uint8_t extpan[8]);
// Record last-known identity when a silent wipe is detected (drives the banner).
void coord_health_mark_lost(uint16_t pan, uint8_t channel);
// Reader-side seqlock copy of extpan (httpd task). false if no stable read.
bool coord_health_read_extpan(uint8_t out[8]);

#ifdef __cplusplus
}
#endif
