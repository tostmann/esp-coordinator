#include "coord_health.h"

#include <string.h>

// pan/channel default to the "unknown" sentinels; valid=false until the first
// ZBOSS publish, so the page shows "(starting)" rather than "PAN 0x0000".
struct coord_health g_coord_health = {
    /*valid*/ false, /*joined*/ false, /*pan*/ 0xFFFF, /*channel*/ 0xFF,
    /*lost_pan*/ 0xFFFF, /*lost_channel*/ 0xFF, /*extpan_seq*/ 0, /*extpan*/ {0},
};

void coord_health_publish(bool joined, uint16_t pan, uint8_t channel,
                          const uint8_t extpan[8]) {
    g_coord_health.joined  = joined;
    g_coord_health.pan     = pan;
    g_coord_health.channel = channel;
    g_coord_health.extpan_seq++;                // -> odd (write in progress)
    memcpy(g_coord_health.extpan, extpan, 8);
    g_coord_health.extpan_seq++;                // -> even (stable)
    g_coord_health.valid = true;
}

void coord_health_mark_lost(uint16_t pan, uint8_t channel) {
    g_coord_health.lost_pan     = pan;
    g_coord_health.lost_channel = channel;
}

bool coord_health_read_extpan(uint8_t out[8]) {
    for (int t = 0; t < 4; ++t) {
        uint32_t s1 = g_coord_health.extpan_seq;
        if (s1 & 1u) continue;                  // writer mid-update
        memcpy(out, g_coord_health.extpan, 8);
        if (g_coord_health.extpan_seq == s1) return true;  // no tear
    }
    return false;
}
