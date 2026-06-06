#include "zb_ncp.h"
#include "commands.h"
#include "protocol.h"
#include "statuses.h"
#include "utils.h"
#include "transport.h"
#include "backup_structured.h"
#include "nwk/esp_zigbee_nwk.h"
#include "esp_zigbee_secur.h"
#include <nvs.h>
// NOTE: deliberately NOT including esp_zigbee_core.h — referencing any of its
// symbols (e.g. esp_zb_set_primary_network_channel_set) pulls in
// esp_zigbee_core.c.obj from libesp_zb_api.zczr.a, which redefines our own
// zboss_signal_handler below and causes a link-time multiple-definition.
// For channel-mask setup during restore we use the zboss-direct zb_set_* APIs
// already wired up via zb_ncp::set_channel_mask.

static const char* TAG = "NCP";

#include "commands_impl.h"

// Set to true by apply_pending_restore() when it actually applies a TLV.
// continue_zboss reads it to force NETWORK_FORMATION (which performs the
// channel-scan step) instead of the default STEERING — without the explicit
// formation kick, ZBOSS reports joined=true but the radio sits on channel
// 255 (uninitialised) because no scan ever ran. See espressif/esp-zigbee-sdk#445
// for the underlying constraint: setters only take effect in factory-reset
// state, so the restore-applied flag also gates a formation pass that uses
// our just-set NIB values rather than re-randomising them.
static bool s_restore_applied = false;

// Pulls "restore_pend"/"tlv" out of NVS (set by RESTORE_STRUCTURED_BACKUP
// before the prior reboot) and replays it into the freshly-initialised
// stack via the esp_zb_set_* / esp_zb_secur_* APIs. Must run after ZB_INIT
// (so esp_zb_set_long_address is legal) and before the stack is on a
// network (so the *_set_frame_counter / *_set_update_id / *_network_key_set
// APIs do not return ESP_ERR_INVALID_STATE).  Blob is erased after a
// successful apply so the restore is one-shot.
//
// Devices joined to the prior network rejoin via Device_annce once the
// coordinator is back on air with the same PAN+ExtPAN+channel+key; the
// neighbor table is rebuilt by ZBOSS, so TAG_DEVICE_TABLE is ignored here.
static void apply_pending_restore() {
    nvs_handle_t nh;
    esp_err_t err = nvs_open("restore_pend", NVS_READWRITE, &nh);
    if (err != ESP_OK) {
        return; // no pending restore is the common path
    }
    size_t blob_size = 0;
    err = nvs_get_blob(nh, "tlv", nullptr, &blob_size);
    if (err != ESP_OK || blob_size < sizeof(backup_structured::header_t)) {
        nvs_close(nh);
        return;
    }
    auto* buf = static_cast<uint8_t*>(malloc(blob_size));
    if (!buf) {
        ESP_LOGE(TAG, "apply_pending_restore: malloc(%u) failed", (unsigned)blob_size);
        nvs_close(nh);
        return;
    }
    err = nvs_get_blob(nh, "tlv", buf, &blob_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_pending_restore: nvs_get_blob failed: %s", esp_err_to_name(err));
        free(buf);
        nvs_close(nh);
        return;
    }
    // Erase + persist BEFORE applying setters — guards against a setter call
    // panicking the chip and entering an infinite crash-loop. If apply crashes
    // here, the next boot finds an empty namespace and boots normally; the
    // network identity is then lost (user must re-restore manually) but the
    // chip is recoverable without reflashing.
    nvs_erase_key(nh, "tlv");
    nvs_commit(nh);
    nvs_close(nh);

    backup_structured::header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (memcmp(hdr.magic, backup_structured::MAGIC, 4) != 0 ||
        hdr.version != backup_structured::VERSION ||
        sizeof(hdr) + hdr.payload_len > blob_size) {
        ESP_LOGE(TAG, "apply_pending_restore: invalid header (magic/version/len)");
        free(buf);
        return;
    }

    ESP_LOGI(TAG, "apply_pending_restore: replaying %u-byte payload", hdr.payload_len);

    const uint8_t* p   = buf + sizeof(hdr);
    const uint8_t* end = p + hdr.payload_len;
    while (p + 3 <= end) {
        uint8_t  tag = *p++;
        uint16_t len;
        memcpy(&len, p, 2);  p += 2;
        if (p + len > end) {
            ESP_LOGE(TAG, "apply_pending_restore: TLV 0x%02x len %u runs past end", tag, len);
            break;
        }
        switch (tag) {
            case backup_structured::TAG_PAN_ID: {
                if (len != 2) break;
                uint16_t pan; memcpy(&pan, p, 2);
                esp_zb_set_pan_id(pan);
                ESP_LOGI(TAG, "  PAN ID = 0x%04x", pan);
            } break;
            case backup_structured::TAG_EXT_PAN_ID: {
                if (len != 8) break;
                esp_zb_ieee_addr_t ext;
                memcpy(ext, p, 8);
                // CRITICAL: esp_zb_set_extended_pan_id() sets apsUseExtendedPANID
                // (the JOIN-target ExtPAN), NOT the NWK's own ExtPAN that the
                // coordinator broadcasts in beacons. For restore we need the NIB-
                // level setter so the formed network advertises this exact ExtPAN
                // and previously-paired devices recognise it.
                // Per espressif/esp-zigbee-sdk#445 (xieqinan), this setter only
                // takes effect when the chip starts in factoryreset state — our
                // RESTORE_STRUCTURED_BACKUP handler enforces that by erasing
                // zb_storage before reboot, so apply_pending_restore runs on a
                // truly fresh stack here.
                esp_zb_nwk_set_extended_pan_id(ext);
                ESP_LOGI(TAG, "  NWK ExtPAN set");
            } break;
            case backup_structured::TAG_CHANNEL: {
                if (len != 1) break;
                uint8_t ch = *p;
                if (ch >= 11 && ch <= 26) {
                    uint32_t mask = 1u << ch;
                    // Use the zboss-direct setters (same path as
                    // zb_ncp::set_channel_mask) — avoids pulling
                    // esp_zigbee_core.c.obj into the link (see include note).
                    zb_set_channel_mask(mask);
                    zb_set_bdb_primary_channel_set(mask);
                    zb_set_bdb_secondary_channel_set(mask);
                    ESP_LOGI(TAG, "  Channel = %u (mask 0x%08lx)", ch, (unsigned long)mask);
                }
            } break;
            case backup_structured::TAG_NWK_UPDATE_ID: {
                if (len != 1) break;
                esp_zb_nwk_set_update_id(*p);
                ESP_LOGI(TAG, "  NWK update id = %u", *p);
            } break;
            case backup_structured::TAG_COORD_IEEE: {
                if (len != 8) break;
                esp_zb_ieee_addr_t ieee;
                memcpy(ieee, p, 8);
                esp_zb_set_long_address(ieee);
                ESP_LOGI(TAG, "  Long addr set");
            } break;
            case backup_structured::TAG_NWK_KEY: {
                if (len != 16) break;
                uint8_t key[16];
                memcpy(key, p, 16);
                esp_zb_secur_network_key_set(key);
                ESP_LOGI(TAG, "  NWK key installed");
            } break;
            case backup_structured::TAG_NWK_FRAME_COUNTER: {
                if (len != 4) break;
                uint32_t fc; memcpy(&fc, p, 4);
                esp_zb_nwk_set_frame_counter(fc);
                ESP_LOGI(TAG, "  Frame counter = %lu", (unsigned long)fc);
            } break;
            case backup_structured::TAG_DEVICE_TABLE:
                // Informational only — ZBOSS rebuilds the neighbor table from
                // Device_annce / NWK Status as devices rejoin.
                break;
            default:
                ESP_LOGW(TAG, "  unknown TLV tag 0x%02x len %u (skipped)", tag, len);
                break;
        }
        p += len;
    }

    free(buf);
    s_restore_applied = true;
    ESP_LOGI(TAG, "apply_pending_restore: done");
}

zb_ncp::zb_ncp() {

}

zb_ncp& zb_ncp::instance() {
	static zb_ncp s_zb_ncp;
	return s_zb_ncp;
}

ZB_DECLARE_SIMPLE_DESC(7,20);

static const zb_af_simple_desc_7_20_t ep1 = {
	
		.endpoint = 1,
		.app_profile_id = ZB_AF_HA_PROFILE_ID,
		.app_device_id = 0xbeef,
		.app_device_version = 1,
		.reserved = 0,
		.app_input_cluster_count = 7,
		.app_output_cluster_count = 20,
		.app_cluster_list = {
			0x0000, 0x0003, 0x0006, 0x000a, 0x0019, 0x001a, 0x0300,
			0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0x0020, 0x0300, 0x0400, 0x0402, 0x0405, 0x0406, 0x0500, 0x0b01, 0x0b03, 0x0b04,
			0x0702, 0x1000, 0xfc01, 0xfc02,
		}
	
};


extern "C" void zb_config_overall_network_size(uint16_t size);

esp_err_t zb_ncp::init_int() {
	zb_config_overall_network_size(OVERALL_NETWORK_SIZE); // Set network size first to scale up tables
	ZB_INIT();
	zb_set_max_children(OVERALL_NETWORK_SIZE);
    zb_set_nvram_erase_at_start(0);
    zb_set_installcode_policy(0);
    zb_tc_set_use_installcode(0);
    //zgp_disable();

    // Replay a pending RESTORE_STRUCTURED_BACKUP payload from NVS, if any.
    // Must run after ZB_INIT (so esp_zb_set_long_address etc. are legal) and
    // before zboss_start_no_autostart() — once the stack reaches the
    // "joined" state from NVRAM, the setters refuse with INVALID_STATE.
    apply_pending_restore();

    zboss_start_no_autostart();

    zb_add_simple_descriptor(&ep1);

    m_channels_mask = zb_get_channel_mask();
    return ESP_OK;
}

void zb_ncp::set_channel_mask(uint32_t mask) {
	instance().m_channels_mask = mask;
	zb_set_channel_mask(mask);
    zb_set_bdb_primary_channel_set(mask);
    zb_set_bdb_secondary_channel_set(mask);
}

static bool s_init_flag = false;

void zb_ncp::ncp_zb_task(void* arg) {
	zb_set_network_coordinator_role(0xffffff);


    zboss_main_loop();
    vTaskDelete(NULL);
}

bool zb_ncp::start_zigbee_stack() {
	if (!s_init_flag) {
            
        ESP_LOGI(TAG,"Start Zigbee task");

        xTaskCreate(&zb_ncp::ncp_zb_task, "ncp_zb_task", ZB_TASK_STACK_SIZE, NULL, 5, NULL);

        s_init_flag = true;
        return true;
    } else {
        return false;
    }
}

// ZBOSS "big lock" — the esp-zboss-lib port layer's recursive mutex
// (s_zb_mutex in zb_esp_osif.c; zb_esp_osif_lock_acquire/release are public
// API, declared in zb_osif_platform.h). The ZBOSS task holds it across every
// scheduler iteration (released only around its select() wait), because the
// ZBOSS core is NOT thread-safe: its interrupt-bracketing primitive is a
// plain global byte (g_dis_inter_flag) around vPortEnter/ExitCritical, with
// a preemption window between the critical exit and the flag clear (verified
// by disassembly 2026-06-06). esp-zigbee-lib's public API enforces the same
// rule via esp_zb_lock_acquire(); the raw-ZBOSS NCP design inherited from
// upstream never took the lock, so every zb_* call our command handlers make
// from the app task raced the ZBOSS task's own brackets. Under interview
// bursts (many host ZDO commands while INDICATIONs stream out) the race
// fired reproducibly on the wifi-coex branch: 'assert failed:
// vPortExitCritical port.c:618 (port_uxCriticalNesting[0] > 0)' on
// ncp_zb_task — 4 panics, all mid-interview; USB timing makes it rarer but
// the race is identical here. The dispatch below is the single app-task
// entry into ZBOSS, so one guard serialises all of it. Lock order on the
// app task becomes s_zb_mutex -> m_tx_sem -> m_input_sem — identical to the
// ZBOSS task's own callback path, so no deadlock is introduced. Validated on
// the branch with a 10/10 interview-hammer (~80% crash rate before).
namespace {
struct zboss_lock_guard {
	zboss_lock_guard() { zb_esp_osif_lock_acquire(portMAX_DELAY); }
	~zboss_lock_guard() { zb_esp_osif_lock_release(); }
};
}  // namespace

void zb_ncp::on_rx_data(const void* data,size_t size) {
	// Serialise against the ZBOSS task before any handler can touch zb_*
	// state (see the big-lock comment above).
	zboss_lock_guard zb_lock;

	// Guard BEFORE dereferencing cmd: a data packet with packet_len 7..11 hands
	// us data_len 0..4 (< sizeof(cmd_t)). Reading cmd.type/command_id from such
	// a sub-header frame OOB-reads the RX-buffer tail, and `size - sizeof(cmd_t)`
	// wraps to ~SIZE_MAX, defeating every helper's `inlen < sizeof(Arg)` check
	// (worst case SET_NWK_KEY pulling 16 key bytes from an uninit buffer).
	// One guard closes DISP-1/HELP-1/SEC-2/GETSET-1/ZDO-2/BACKUP-4.
	if (size < sizeof(cmd_t)) {
		ESP_LOGE(TAG,"cmd frame too short: %u",unsigned(size));
		return;
	}
	const cmd_t& cmd = *static_cast<const cmd_t*>(data);
	if (cmd.type != REQUEST && cmd.type != RESPONSE) {
		ESP_LOGE(TAG,"Indication received from host");
	 	return;
	}
	auto len = size - sizeof(cmd_t);
	auto buf = static_cast<const uint8_t*>(data)+sizeof(cmd_t);

    switch(cmd.command_id) {
#define COMMAND(Name,Val) \
        case Name: \
            cmd_handle<Name>::process(cmd, buf, len); \
            break;
#include "commands_list.h"
#undef COMMAND
        default: {
            //ret = ESP_ERR_INVALID_ARG; 
            ESP_LOGE(TAG,"unknown cmd %04x",cmd.command_id);
            //esp_ncp_resp_input(ncp_header, invalid_cmd_resp, 2); 
            uint8_t outdata[2+sizeof(zb_ncp::cmd_t)];
	        zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
	        *out_cmd = cmd;
	       	out_cmd->type = RESPONSE;
	       	outdata[0+sizeof(zb_ncp::cmd_t)] = STATUS_CATEGORY_GENERIC;
	       	outdata[1+sizeof(zb_ncp::cmd_t)] = GENERIC_NOT_IMPLEMENTED;
	       	zb_ncp::send_cmd_data( outdata, sizeof(outdata) ); 
        } break;
    }

}

void zb_ncp::send_cmd_data(const void* data,size_t size) {
	const auto cmd = static_cast<const cmd_t*>(data);
	ESP_LOGD(TAG,"Send cmd data: %04x",cmd->command_id);
	auto res = protocol::send_data( data, size ); 
	if (res != ESP_OK) {
		ESP_LOGE(TAG,"Failed send data");
	}
}
void zb_ncp::indication(command_id_t cmd,const void* data,size_t size) {
	// 512 covers the worst-case APSDE_DATA_IND: data_indication() builds a
	// ~24-byte header followed by up to 255 bytes of ZCL payload (~279 total),
	// and we still need slack here for the 4-byte indication prefix and any
	// future indication types with larger payloads. Pre-fix 256 silently
	// dropped any indication with > 252 bytes of payload — i.e. OTA image
	// blocks and multi-attribute reads vanished without the host noticing.
	uint8_t buffer[512];
	if ((size+4) > sizeof(buffer)) {
		ESP_LOGE(TAG,"Indication too long: %zu", size+4);
		return;
	}
	buffer[0] = 0;
	buffer[1] = INDICATION;
	// PROTO-3: buffer+2 is an unaligned offset; an unaligned uint16_t store is
	// UB (works on C6 SRAM, but keep it clean). memcpy the 16-bit command_id.
	uint16_t cmd16 = static_cast<uint16_t>(cmd);
	memcpy(&buffer[2], &cmd16, sizeof(cmd16));
	memcpy(&buffer[4],data,size);
	auto res = protocol::send_data( buffer, size+4 ); 
	if (res != ESP_OK) {
		ESP_LOGE(TAG,"Failed send indication");
	}
}

static zb_uint8_t data_indication(zb_bufid_t param) {
  zb_apsde_data_indication_t *ind = ZB_BUF_GET_PARAM(param, zb_apsde_data_indication_t);
  static_assert(sizeof(zb_apsde_data_indication_t)==0x20);
 
  auto begin = static_cast<const uint8_t*>(zb_buf_begin(param));
  auto len = zb_buf_len(param);

  ESP_LOGD(TAG,"data_indication profileId: %04x clusterId: %04x srcAddr: %04x "
    " dstAddr: %04x srcEndpoint: %d dstEndpoint: %d data: %d tsn: %d",
    ind->profileid,ind->clusterid,ind->src_addr,ind->dst_addr,
    int(ind->src_endpoint),int(ind->dst_endpoint),int(len),ind->tsn);
 
  if (ind->src_endpoint == 0) {
    // ZDP frame from a remote device. Most clusters we let the ZBOSS stack
    // parse normally — but cluster 0x8004 (Simple_Desc_rsp) needs a tolerant
    // intercept because the stack's strict frame-length check drops some
    // Tuya devices' malformed responses (see zb_ncp.h for the issue link).
    if (ind->clusterid == 0x8004 && begin && len >= 5) {
      if (zb_ncp::try_intercept_simple_desc_rsp(begin, len)) {
        ESP_LOGD(TAG, "Simple_Desc_rsp intercepted (tsn=%d), bypassing stack parser", int(begin[0]));
        zb_buf_free(param);
        return ZB_TRUE;
      }
    }
    ESP_LOGD(TAG,"skip, %d %d",int(ind->src_endpoint),int(ind->dst_endpoint));
    return ZB_FALSE;
  }

  // if (begin) {
  //   ESP_LOGI(TAG,"<<<<< IND");
  //   ESP_LOG_BUFFER_HEX_LEVEL(TAG, begin, len, ESP_LOG_INFO);
  //   ESP_LOGI(TAG,"====");
  // }


  if (len <= 255) {
      uint8_t outdata[8+8*2+255];

      uint8_t* out = outdata;
      auto write_u16 = [&out](uint16_t val) {
        // PROTO-3: `out` advances by mixed u8/u16 steps so it is frequently
        // unaligned — memcpy avoids the UB of an unaligned uint16_t store.
        memcpy(out, &val, 2);
        out += 2;
      };
      auto write_u8 = [&out](uint8_t val) {
        *out++ = val;
      };
      write_u8(0);                  // {name: 'paramLength', type: DataType.UINT8},
      write_u16(len);               // {name: 'dataLength', type: DataType.UINT16},
      write_u8(ind->fc);            //  {name: 'apsFC', type: DataType.UINT8},
      write_u16(ind->src_addr);     //  {name: 'srcNwk', type: DataType.UINT16},
      write_u16(ind->dst_addr);     //  {name: 'dstNwk', type: DataType.UINT16},
      write_u16(ind->group_addr);   //  {name: 'grpNwk', type: DataType.UINT16},

      write_u8(ind->dst_endpoint); //  {name: 'dstEndpoint', type: DataType.UINT8},
      write_u8(ind->src_endpoint); //  {name: 'srcEndpoint', type: DataType.UINT8},
      write_u16(ind->clusterid); //  {name: 'clusterID', type: DataType.UINT16},
      write_u16(ind->profileid); //  {name: 'profileID', type: DataType.UINT16},
      write_u8(ind->aps_counter); //  {name: 'apsCounter', type: DataType.UINT8},
      write_u16(ind->mac_src_addr); //  {name: 'srcMAC', type: DataType.UINT16},
      write_u16(ind->mac_dst_addr); //  {name: 'dstMAC', type: DataType.UINT16},
      write_u8(ind->lqi); //  {name: 'lqi', type: DataType.UINT8},
      write_u8(ind->rssi); //  {name: 'rssi', type: DataType.UINT8},
      write_u8(ind->aps_key_source|(ind->aps_key_attrs<<1)|(ind->aps_key_from_tc<<3)|(ind->extended_fc<<4)); //  {name: 'apsKey', type: DataType.UINT8},
      
      ::memcpy(out,begin,len);
      out += len;


      zb_ncp::indication(APSDE_DATA_IND,outdata,out-&outdata[0]);
  } else {
    ESP_LOGE(TAG,"too long packet");
  }

  zb_buf_free(param);
  return ZB_TRUE;
}

void zb_ncp::continue_zboss(uint8_t arg) {
    ESP_LOGI(TAG,"continue_zboss");

    // Cold-boot race fix (andryblack/esp-coordinator#5 + regression in #19).
    // The synthetic NCP_RESET response (cmd=0x0002, type=RESPONSE, tsn=0xFF,
    // status=OK) used to be sent unconditionally from app::start_int() at
    // boot, BEFORE ZBOSS finished loading its NVRAM dataset. z2m's first
    // getNetworkInfo() call (driver.js:152, sequence GET_JOINED →
    // GET_ZIGBEE_ROLE → GET_LOCAL_IEEE_ADDR → GET_EXTENDED_PAN_ID →
    // GET_PAN_ID → GET_ZIGBEE_CHANNEL) then returned the uninitialised
    // 0xFFFF / 0xFF defaults; z2m compared against its configured options,
    // found a mismatch, and called formNetwork() — wiping every paired
    // device. We send the boot-ready frame here instead because
    // ZB_ZDO_SIGNAL_SKIP_STARTUP is the earliest stack signal at which the
    // NVRAM dataset is guaranteed loaded; zb_get_pan_id() and friends now
    // return the persisted values by the time z2m queries them. PR#6 in
    // upstream andryblack/esp-coordinator moved zboss_start_no_autostart()
    // into init_int() (we have that) but the dataset load itself runs on
    // the dedicated ZBOSS task inside zboss_main_loop, so PR#6 alone left
    // a race window — closed by deferring the boot-ready frame.
    static const uint8_t boot_ready_frame[] = {
        0xDE, 0xAD,             // signature
        0x0E, 0x00,             // packet_len = 14
        0x06,                   // packet_type = ZBOSS_NCP_API_HL
        0xC0,                   // flags: first_fragment=1, last_fragment=1
        0x5D,                   // header CRC8
        0x50, 0xD4,             // payload CRC16
        0x00,                   // version
        0x01,                   // type = RESPONSE
        0x02, 0x00,             // command_id = NCP_RESET (0x0002, LE)
        0xFF,                   // tsn = 0xFF (unsolicited-boot sentinel)
        0x00, 0x00              // CATEGORY_GENERIC, GENERIC_OK
    };
    transport::send(boot_ready_frame, sizeof(boot_ready_frame));

    zb_af_set_data_indication(data_indication);

    ESP_LOGI(TAG,"continue_zboss 1");
    // ncp_cmd_handle<S_ESP_NCP_NETWORK_INIT>::response(0);

    if (cmd_handle<NWK_FORMATION>::need_resolve()) {
        set_channel_mask(instance().m_channels_mask);
        auto res = bdb_start_top_level_commissioning(ZB_BDB_NETWORK_FORMATION);
        if (!res) {
            cmd_handle<NWK_FORMATION>::response(GENERIC_ERROR);
        }
    } else if (s_restore_applied) {
        // After apply_pending_restore: PAN/ExtPAN/Key/UpdateID are in the NIB
        // but the radio has not yet selected an operating channel (channel scan
        // hasn't run). STEERING is a no-op for an "already-joined" coord, so
        // explicitly trigger FORMATION — the configured channel mask narrows
        // the scan to the restored channel and the NIB values we just set
        // survive because we are in factory-reset state per #445.
        ESP_LOGI(TAG, "continue_zboss: restore applied, forcing NETWORK_FORMATION");
        set_channel_mask(instance().m_channels_mask);
        bdb_start_top_level_commissioning(ZB_BDB_NETWORK_FORMATION);
    } else {
        bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
    }
}


extern "C" void zboss_signal_handler(zb_uint8_t param)
{
    zb_zdo_app_signal_hdr_t *sg_p = NULL;
    zb_zdo_app_signal_type_t sig = zb_get_app_signal(param, &sg_p);

    int status = ZB_GET_APP_SIGNAL_STATUS(param);
    bool success = status == 0;

  
    switch (sig)
    {
    case ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGD(TAG,"ZB_ZDO_SIGNAL_SKIP_STARTUP");
      
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        
        
        zb_schedule_app_callback(zb_ncp::continue_zboss,0);
        
        
        break;
    case ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (success) {
            ESP_LOGI(TAG, "Device reboot complete");
            //zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_FORMATION);
        } else {
            ESP_LOGE(TAG, "Failed to initialize Zigbee stack (status: %d)", status);

        }
        zb_ncp::cmd_handle<NWK_START_WITHOUT_FORMATION>::response(success ? 0 : 1);

        break;
    case ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        ESP_LOGD(TAG,"ZB_ZDO_SIGNAL_DEVICE_ANNCE");
        auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const zb_zdo_signal_device_annce_params_t);
        zb_ncp::indication(ZDO_DEV_ANNCE_IND,parameters,sizeof(zb_zdo_signal_device_annce_params_t));
    } break;
    case ZB_ZDO_SIGNAL_LEAVE: {
        auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const zb_zdo_signal_leave_params_t);
        ESP_LOGD(TAG,"ZB_ZDO_SIGNAL_LEAVE %02x",int(parameters->leave_type));
        //esp_ncp_noti_input(NWK_LEAVE_IND,parameters,sizeof(zb_zdo_signal_leave_params_t));
    }    break;
    case ZB_ZDO_SIGNAL_ERROR:
        ESP_LOGE(TAG,"ZB_ZDO_SIGNAL_ERROR");
        break;
    case ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const zb_zdo_signal_leave_indication_params_t);
        ESP_LOGD(TAG,"ZB_ZDO_SIGNAL_LEAVE_INDICATION");
        zb_ncp::indication(NWK_LEAVE_IND,parameters,sizeof(zb_zdo_signal_leave_indication_params_t));
    } break;
    case ZB_ZDO_DEVICE_UNAVAILABLE: {
        auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const zb_zdo_device_unavailable_params_t);
        ESP_LOGD(TAG,"ZB_ZDO_DEVICE_UNAVAILABLE %04x",parameters->short_addr);
    }break;

    case ZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        ESP_LOGI(TAG,"ZB_ZDO_SIGNAL_DEVICE_UPDATE");
        auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const zb_zdo_signal_device_update_params_t);
        ESP_LOGD(TAG,"addr: %04x status: %d parent: %04x",parameters->short_addr,int(parameters->status),parameters->parent_short);

    
        zb_ncp::indication(ZDO_DEV_UPDATE_IND,parameters,sizeof(zb_zdo_signal_device_update_params_t));
        // {name: 'ieee', type: DataType.IEEE_ADDR},
        // {name: 'nwk', type: DataType.UINT16},
        // {name: 'status', type: DataType.UINT8, typed: DeviceUpdateStatus},
    }break;

    case ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        
        auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const zb_zdo_signal_device_authorized_params_t);
        ESP_LOGD(TAG,"ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED long_addr: " IEEE_ADDR_FMT " short_addr: %04x auth_type: %d auth_status: %d",
            IEEE_ADDR_PRINT(parameters->long_addr),parameters->short_addr,
            int(parameters->authorization_type),int(parameters->authorization_status));

    
        zb_ncp::indication(ZDO_DEV_AUTHORIZED_IND,parameters,sizeof(zb_zdo_signal_device_authorized_params_t));
    } break;

    case ZB_BDB_SIGNAL_STEERING:
        ESP_LOGD(TAG,"ZB_BDB_SIGNAL_STEERING");
        if (success) {
            ESP_LOGI(TAG, "Network steering started");
        }
        break;
    case ZB_BDB_SIGNAL_FORMATION:
        ESP_LOGD(TAG,"ZB_BDB_SIGNAL_FORMATION");
        zb_ncp::cmd_handle<NWK_FORMATION>::response(status);
        if (success) {
            bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
            ESP_LOGI(TAG, "Formed network successfully");
        } else {
            
        }
 
        break;

    

    case ZB_BDB_SIGNAL_FINDING_AND_BINDING_TARGET_FINISHED:
        ESP_LOGD(TAG,"ZB_BDB_SIGNAL_FINDING_AND_BINDING_TARGET_FINISHED");
        break;
    case ZB_BDB_SIGNAL_FINDING_AND_BINDING_INITIATOR_FINISHED:
        ESP_LOGD(TAG,"ZB_BDB_SIGNAL_FINDING_AND_BINDING_INITIATOR_FINISHED");
        break;
    case ZB_NWK_SIGNAL_DEVICE_ASSOCIATED: {
        auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const zb_nwk_signal_device_associated_params_t);
        ESP_LOGD(TAG,"ZB_NWK_SIGNAL_DEVICE_ASSOCIATED addr: " IEEE_ADDR_FMT,IEEE_ADDR_PRINT(parameters->device_addr));
        break;
    }
    
    case ZB_BDB_SIGNAL_WWAH_REJOIN_STARTED:
        ESP_LOGD(TAG,"ZB_BDB_SIGNAL_WWAH_REJOIN_STARTED");
        break;
    case ZB_ZGP_SIGNAL_COMMISSIONING:
        ESP_LOGD(TAG,"ZB_ZGP_SIGNAL_COMMISSIONING");
        break;
    case ZB_COMMON_SIGNAL_CAN_SLEEP:
        ESP_LOGD(TAG,"ZB_COMMON_SIGNAL_CAN_SLEEP");
        break;
    case ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
        ESP_LOGD(TAG,"ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY");
        break;
    case ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        ESP_LOGD(TAG,"ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT");
        break;
    case ZB_NLME_STATUS_INDICATION: {
        auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const zb_zdo_signal_nlme_status_indication_params_t);
        ESP_LOGE(TAG,"ZB_NLME_STATUS_INDICATION %04x %s(%d)",parameters->nlme_status.network_addr,
                utils::get_nlme_status_str(parameters->nlme_status.status),
                int(parameters->nlme_status.status));
        if (parameters->nlme_status.status == ZB_NWK_COMMAND_STATUS_UNKNOWN_COMMAND) {
            ESP_LOGE(TAG,"unknown command: %04x",int(parameters->nlme_status.unknown_command_id));
        }
        break;
    }
    

    case ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        ESP_LOGD(TAG,"ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS");
        if (success) {
            auto parameters = ZB_ZDO_SIGNAL_GET_PARAMS(sg_p,const uint8_t);
            if (*parameters) {
                ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds", zb_get_pan_id(), *parameters);
            } else {
                ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", zb_get_pan_id());
            }
            //ncp_header.id = ESP_NCP_NETWORK_PERMIT_JOINING;
            //esp_ncp_noti_input(&ncp_header, parameters, sizeof(uint8_t));
        }
        break;

    
        
    default:
        ESP_LOGE(TAG,"unknown signal: %d %d",sig,status);
        break;
        
    }
    zb_buf_free(param);
}


extern "C" bool zb_zcl_green_power_cluster_handler(zb_uint8_t param) {
    // ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    // ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
    //                     message->info.status);
    // ESP_LOGI(TAG, "Received ZGP %s message: endpoint(%d), cluster(0x%x), command(0x%x)", message->info.command.direction == 1 ? "proxy" : "sink",
    //          message->info.dst_endpoint, message->info.cluster, message->info.command.id);
    ESP_LOGW(TAG,"Green power cluster handle");
    return true;
}


// ---------------------------------------------------------------------------
// Simple_Desc_rsp intercept — bypasses libzboss_stack.zczr.a's strict
// frame-length check on cluster 0x8004 (see header comment in zb_ncp.h).
//
// ZDP wire format for Simple_Desc_rsp (zigbee spec 2.4.4.1.5):
//   TSN        (1)   ← matched against pending request
//   STATUS     (1)
//   NWK_ADDR   (2, little-endian)
//   LENGTH     (1)   ← declared length of the simple_desc body
//   SIMPLE_DESC (LENGTH bytes):
//     endpoint        (1)
//     profile_id      (2)
//     device_id       (2)
//     device_version  (low 4 bits) + reserved (high 4 bits)  → packed in (1)
//     in_count        (1)
//     in_cluster_list (2 * in_count)
//     out_count       (1)
//     out_cluster_list(2 * out_count)
// Some Tuya devices ship trailing extra bytes; we ignore anything past LENGTH.
//
// Output mirrors cmd_handle<ZDO_SIMPLE_DESC_REQ>::format_response (in
// commands_impl.h:769-799) so the host (zigbee-herdsman zboss adapter) decodes
// it identically to a stack-parsed response.
bool zb_ncp::try_intercept_simple_desc_rsp(const uint8_t* p, uint16_t len) {
    if (!p || len < 5) return false;

    const uint8_t  tsn         = p[0];
    const uint8_t  status      = p[1];
    uint16_t       nwk_addr    = 0;
    memcpy(&nwk_addr, p + 2, 2);
    const uint8_t  declared_sd = p[4];

    using Resolver = request_cmd_resolver<ZDO_SIMPLE_DESC_REQ, zb_zdo_simple_desc_req_t>;
    // DISP-2: claim+free the slot atomically up front. We then build and send the
    // synthesised response (a blocking send_cmd_data) on the local copy, so the
    // app task can't re-pick this slot mid-send. resolve_take already does what
    // the old trailing `req->state = S_NONE` did — free the slot so the late
    // ZBOSS TIMEOUT callback (~20 s out, after APS retries exhaust) finds no
    // match and is silently dropped (req_cb's "not found" path).
    Resolver::request_t req;
    if (!Resolver::resolve_take(tsn, req)) {
        // No pending request matches this tsn — either already handled by
        // somebody else, or stale. Let the stack continue (caller will return
        // false to ZBOSS) so its normal error path runs.
        return false;
    }

    // Number of bytes we can actually read for the simple_desc body. Trust the
    // smaller of (frame remainder) and (declared length) — the whole point of
    // the intercept is to tolerate frame-length > declared. We also accept
    // frame-length < declared (truncate to what we have) since some devices
    // are buggy in the other direction too.
    uint16_t avail = (len > 5) ? (len - 5) : 0;
    if (declared_sd < avail) avail = declared_sd;

    // Build the response wire format directly. Layout (per format_response):
    //   cmd_t (4) | CATEGORY_ZDO (1) | status (1) | endpoint (1)
    //   | profile_id (2) | device_id (2) | version (1) | in_count (1)
    //   | out_count (1) | in_clusters (2*in_count) | out_clusters (2*out_count)
    //   | nwk_addr (2)
    // With in_count + out_count capped at 32, max body = 8 + 2*32 = 72 bytes
    // plus 2 status header + 2 nwk + cmd_t = 80 bytes. Reserve 128 for slack.
    uint8_t outdata[128];
    auto out_cmd = reinterpret_cast<cmd_t*>(outdata);
    *out_cmd       = req.cmd;
    out_cmd->type  = RESPONSE;

    uint8_t* out = reinterpret_cast<uint8_t*>(out_cmd + 1);
    *out++ = STATUS_CATEGORY_ZDO;
    *out++ = status;

    if (status == 0 && avail >= 7) {
        // Wire-format layout (ZCL Foundation 2.4.4.1.5 Simple_Desc_rsp body):
        //   sd[0]            endpoint
        //   sd[1..2]         profile_id  (little-endian)
        //   sd[3..4]         device_id   (little-endian)
        //   sd[5]            device_version (low 4 bits) | reserved (high 4 bits)
        //   sd[6]            input_cluster_count          (= N)
        //   sd[7 .. 7+2N-1]  input_clusters (each LE u16)
        //   sd[7+2N]         output_cluster_count         (= M)
        //   sd[8+2N .. ]     output_clusters (each LE u16)
        // Total = 8 + 2*(N+M) bytes.  IMPORTANT: input_count and output_count
        // are NOT adjacent on the wire — they're separated by the input list.
        // ZBOSS' internal zb_af_simple_desc_*_t struct stores them adjacently
        // with a single consolidated cluster_list, but that's after parsing.
        const uint8_t* sd = p + 5;
        const uint8_t  endpoint     = sd[0];
        uint16_t       profile_id   = 0;  memcpy(&profile_id, sd + 1, 2);
        uint16_t       device_id    = 0;  memcpy(&device_id,  sd + 3, 2);
        const uint8_t  version_byte = sd[5] & 0x0F;  // low 4 bits, strip reserved
        uint8_t        in_count     = sd[6];

        // Bound input_count to remaining bytes.
        uint16_t bytes_after_in_count = (avail > 7) ? (avail - 7) : 0;
        if (uint16_t(in_count) * 2u > bytes_after_in_count) {
            in_count = bytes_after_in_count / 2;
        }

        // out_count is at offset 7 + 2*in_count (after the input list).
        const uint16_t out_count_off = 7u + 2u * uint16_t(in_count);
        uint8_t  out_count = 0;
        if (avail > out_count_off) {
            out_count = sd[out_count_off];
            uint16_t bytes_after_out_count = (avail > out_count_off + 1u)
                                           ? (avail - out_count_off - 1u) : 0;
            if (uint16_t(out_count) * 2u > bytes_after_out_count) {
                out_count = bytes_after_out_count / 2;
            }
        }

        // Cap at 32 each to fit the response buffer (format_response policy).
        while ((in_count + out_count) > 32) {
            if (in_count > out_count) --in_count; else --out_count;
        }

        *out++ = endpoint;
        memcpy(out, &profile_id, 2); out += 2;
        memcpy(out, &device_id,  2); out += 2;
        *out++ = version_byte;
        *out++ = in_count;
        *out++ = out_count;
        // Input cluster list starts at sd[7].
        for (uint8_t i = 0; i < in_count; ++i) {
            memcpy(out, sd + 7 + 2u * i, 2); out += 2;
        }
        // Output cluster list starts at sd[8 + 2*in_count] (one byte past the
        // input list, accounting for the out_count byte itself).
        for (uint8_t i = 0; i < out_count; ++i) {
            memcpy(out, sd + 8 + 2u * uint16_t(in_count) + 2u * i, 2); out += 2;
        }
    }
    // Tail: nwk_addr is present regardless of status (format_response writes it
    // unconditionally; on error responses simple_desc body is omitted).
    memcpy(out, &nwk_addr, 2); out += 2;

    zb_ncp::send_cmd_data(outdata, out - outdata);

    // Slot was already freed atomically by resolve_take above (DISP-2).
    return true;
}
