#pragma once

// Wire format for the GET_STRUCTURED_BACKUP (0x009B) and
// RESTORE_STRUCTURED_BACKUP (0x009C) NCP commands.
//
// Image layout (after the generic_response_t / cmd_t framing):
//
//   Header (8 bytes):
//     magic[4] = 'Z','B','S','B'
//     version  = uint8 (current = 1)
//     flags    = uint8 (reserved, must be 0)
//     payload_len = uint16 LE (size of the TLV section that follows)
//
//   TLV section: sequence of records, each record:
//     tag : uint8
//     len : uint16 LE
//     value: len bytes
//
// All multi-byte integers are little-endian. Image is processed strictly
// sequentially — host MUST NOT reorder TLVs across the device-table boundary
// since RESTORE applies them in arrival order.
//
// TC link key is intentionally not part of the image (no public getter in
// esp-zigbee-lib). Restore reuses the well-known ZigBeeAlliance09 constant;
// custom-TC-keyed networks require manual re-pair of affected devices.
// APS per-peer frame counters are likewise not exported — devices that
// strictly enforce APS-level replay rejection may briefly re-sync.

#include <cstdint>

namespace backup_structured {

static constexpr uint8_t MAGIC[4] = { 'Z', 'B', 'S', 'B' };
static constexpr uint8_t VERSION  = 1;

struct header_t {
    uint8_t  magic[4];
    uint8_t  version;
    uint8_t  flags;
    uint16_t payload_len;
} __attribute__((packed));
static_assert(sizeof(header_t) == 8);

enum tlv_tag_t : uint8_t {
    TAG_PAN_ID            = 0x01,  // uint16 LE
    TAG_EXT_PAN_ID        = 0x02,  // 8 bytes, ZBOSS-native byte order
    TAG_CHANNEL           = 0x03,  // uint8
    TAG_NWK_UPDATE_ID     = 0x04,  // uint8
    TAG_COORD_IEEE        = 0x05,  // 8 bytes
    TAG_NWK_KEY           = 0x06,  // 16 bytes
    TAG_NWK_KEY_SEQ       = 0x07,  // uint8
    TAG_NWK_FRAME_COUNTER = 0x08,  // uint32 LE
    TAG_DEVICE_TABLE      = 0x10,  // array of device_record_t
};

struct device_record_t {
    uint8_t  ieee[8];
    uint16_t short_addr;
    uint8_t  rx_on_when_idle;  // 0/1
    uint8_t  relationship;     // ZB_NWK_RELATIONSHIP_*
    uint8_t  device_type;      // 0=coord, 1=router, 2=end-device
    uint8_t  depth;
    uint8_t  lqi;              // informational
    uint8_t  reserved;
} __attribute__((packed));
static_assert(sizeof(device_record_t) == 16);

// Upper bound on the device-table TLV's payload. Matches the ZBOSS neighbor
// iteration cap; large networks beyond this need a chunked variant (not
// implemented in v1.2.0).
static constexpr size_t MAX_DEVICES = 64;

// 8 + (network section ~80) + 3 + MAX_DEVICES*16 + a little slack.
static constexpr size_t MAX_IMAGE_SIZE = 8 + 80 + 3 + MAX_DEVICES * 16 + 16;

} // namespace backup_structured
