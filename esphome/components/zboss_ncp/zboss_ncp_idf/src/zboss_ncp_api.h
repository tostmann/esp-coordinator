#pragma once
// Public entry points of the zboss_ncp IDF component (used by the ESPHome
// component in components/zboss_ncp/).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // True when the host network is up (IP assigned). Called from the NCP's
    // TCP task; must be thread-safe (ESPHome publishes an atomic flag).
    bool (*network_up)(void);
    // allowed_ips filter: peer IPv4 in network byte order. NULL = allow all.
    bool (*peer_allowed)(uint32_t ipv4_be);
    // ESPHome runs WiFi: enable WiFi/802.15.4 software coexistence.
    bool use_wifi;
} ncp_host_hooks_t;

// Set before zboss_ncp_main(); the struct is copied.
void ncp_host_set_hooks(const ncp_host_hooks_t* hooks);

// Upstream app_main(): boot guard, pending erase, NCP init and the event pump.
// Does not return while the NCP runs; call it on a task of its own.
void zboss_ncp_main(void);

// True once the radio may be shared: the Zigbee stack is up and coexistence
// enabled (or the NCP is in safe mode, without a stack). Start WiFi only then.
bool zboss_ncp_radio_ready(void);

// Enable WiFi/802.15.4 coexistence again (call once WiFi has started).
void ncp_host_reapply_coex(void);

// Status for ESPHome sensors (read-only snapshots, safe from any task).
typedef struct {
    bool     valid;          // stack has published its identity at least once
    bool     joined;
    uint16_t pan_id;         // 0xFFFF = none
    uint8_t  channel;        // 0xFF = unknown
    uint8_t  ext_pan_id[8];  // as ZBOSS returns it (little-endian)
    bool     network_lost;   // stored network vanished (silent-wipe alarm)
    bool     host_connected; // a host (z2m/ZHA) is the active TCP client
    bool     safe_mode;      // boot guard: ZBOSS not started
} ncp_status_t;

void ncp_get_status(ncp_status_t* out);

const char* zboss_ncp_version(void);

#ifdef __cplusplus
}
#endif
