#include "esp_partition.h"
#include "nvs_flash.h"
#include "app.h"
#include "boot_guard.h"
#include "version.h"

#include "commands_helpers.h"
#include <esp_mac.h>
// (soc/usb_serial_jtag_reg.h include dropped 2026-06-05 — it only served the
// removed USB phy-detach register writes, see the comment in
// ncp_reset_deferred_task.)


// default implementation
template <command_id_t CmdId>
struct zb_ncp::cmd_handle : immediate_cmd_process<CmdId> {
	static constexpr size_t resp_buffer_size = 2;
	static size_t process_immediate(const void *inbuffer, size_t inlen,uint8_t* outdata,size_t outdata_size) {
		ESP_LOGE(TAG,"Unimplemented cmd: %04x",CmdId);
		outdata[0] = STATUS_CATEGORY_GENERIC;
		outdata[1] = GENERIC_NOT_IMPLEMENTED;
        return 2;
    }
};


// [CommandId.GET_MODULE_VERSION]: {
//         request: [],
//         response: [
//             ...commonResponse,
//             {name: 'fwVersion', type: DataType.UINT32},
//             {name: 'stackVersion', type: DataType.UINT32},
//             {name: 'protocolVersion', type: DataType.UINT32},
//         ],
//     },

struct GET_MODULE_VERSION_resp_t {
	uint32_t fwVersion;
	uint32_t stackVersion;
	uint32_t protocolVersion;
} __attribute__((packed)) __attribute__ ((aligned(1)));
template <>
struct zb_ncp::cmd_handle<GET_MODULE_VERSION> : immediate_cmd_process<GET_MODULE_VERSION>,
		general_status_res<GET_MODULE_VERSION,GET_MODULE_VERSION_resp_t> {
	static void process_status_res(ncp_generic_status_t& status,GET_MODULE_VERSION_resp_t* res) {
    	// Encode firmware build into the uint32 Z2M shows as fwVersion.
    	// Layout: MAJOR.MINOR.BUILD with 16-bit BUILD; v1.1.0 = 0x01010000.
    	// Sourced from main/version.h (configure_file from version.h.in).
    	res->fwVersion = (uint32_t(FW_VERSION_MAJOR) << 24)
    	               | (uint32_t(FW_VERSION_MINOR) << 16)
    	               | (uint32_t(FW_VERSION_BUILD) & 0xFFFF);
        res->stackVersion = zboss_version_get();
        res->protocolVersion = ZB_PROTOCOL_VERSION;
    }
};

// Force NCP module reboot
// [CommandId.NCP_RESET]: {
//     request: [{name: 'options', type: DataType.UINT8, typed: ResetOptions}],
//     response: [...commonResponse],
// },

// Reset work that must happen AFTER the matching-tsn response is on the wire.
// z2m's herdsman ZBOSSDriver.reset() throws "{commandId:2} after 10000ms" if
// the response doesn't come back within 10 s. Pre-fix, process_status_arg ran
// zb_nvram_erase() + zb_bdb_reset_via_local_action() inline — both can block
// for seconds (network LEAVE broadcast, flash erase) — before send_cmd_data
// was even called, so the response was queued behind work that exceeded the
// host's timeout window. The reset is now offloaded to a separate FreeRTOS
// task with a short head-start, so process_status_arg returns immediately and
// the response goes out via the event loop.
struct ncp_reset_args_t {
    uint8_t options;
};
static void ncp_reset_deferred_task(void* arg) {
    auto* a = static_cast<ncp_reset_args_t*>(arg);
    uint8_t options = a->options;
    delete a;
    // Yield long enough for the response to drain through the event loop
    // and out the USB-Serial/JTAG ring. 300 ms is generous — actual TX of
    // the ~18-byte response is sub-millisecond — but cheap insurance.
    vTaskDelay(pdMS_TO_TICKS(300));
    // NO flash access here — neither ZBOSS APIs nor raw esp_partition.
    // Both variants were observed to freeze the chip when invoked from
    // this task while the 802.15.4 radio is active (2026-06-05, zigpy-zboss
    // validation: zb_nvram_erase() AND a raw esp_partition_erase_range
    // each stalled with caches disabled — total silence incl. LL-ACKs,
    // only a power-cycle/USB-reset recovers; USB stays enumerated because
    // USB-Serial-JTAG enumerates in hardware). zigpy's write_network_info()
    // always begins with NCP_RESET(FactoryReset), so the entire ZHA
    // auto_form path wedged here. herdsman never hit it because it resets
    // with options=0. The erase request is parked in RTC-noinit RAM
    // (survives esp_restart, zero flash ops pre-reset) and executed in
    // app_main() on the next boot, before the stack/radio starts — see
    // handle_pending_erase() in main.cpp.
    if (options == 1 || options == 2) {
        ESP_LOGI(TAG,"park erase request for next boot (options=%d)", options);
        extern uint32_t g_pending_ncp_erase;
        g_pending_ncp_erase = 0xFAC70000u | options;
    }
    // WEDGE-1: a host-commanded reset is a supervised restart — clear the
    // early-boot failure breadcrumb so a stick parked in boot-guard safe
    // mode exits it on this reboot (with freshly erased NVRAM when
    // options=2, i.e. the remote un-wedge path).
    boot_guard::clear();
    // Force a USB disconnect before esp_restart so the host's CDC layer
    // sees the device drop off the bus. ESP32-C6's USB-Serial-JTAG endpoint
    // stays enumerated across esp_restart() (the ROM bootloader re-attaches
    // the same descriptor instantly), the host never raises a port-close
    // event, and zigbee-herdsman's ZBOSSUart.inReset flag (uart.js:33) is
    // never cleared from onPortClose (uart.js:176) — so every inbound frame
    // gets silently dropped in onPackage (uart.js:183).
    //
    // Disabling DP_PULLUP + USB_PAD_ENABLE collapses the D+ line to 0,
    // which the host recognises as a USB disconnect within ~10 ms.  The
    // 800 ms hold is well above any USB hub debounce and beyond what
    // node-serialport's hot-plug detector needs.  Same pattern Espressif
    // uses in esp-iot-solution/.../boot_hooks.c to hide the USB identity.
    // The chip's reset clears the override + restores defaults (PAD_ENABLE
    // / DP_PULLUP), and the host re-enumerates the device.
    //
    // NOTE: this only helps herdsman get out of inReset.  It does NOT
    // resolve the matching-tsn problem in execCommand — z2m sends
    // NCP_RESET with tsn=X, but our post-reboot boot-ready frame uses
    // the unsolicited-boot sentinel tsn=0xFF.  herdsman's waitressValidator
    // (driver.js:259) requires matcher.tsn === payload.tsn, so the boot
    // frame doesn't satisfy the pending wait.  Real close needs a patch on
    // the tostmann/zigbee2mqtt-esp32 herdsman fork to either accept tsn=0xFF
    // as a wildcard match for NCP_RESET or to resolve pending NCP_RESET
    // promises on inReset clear.  Filed as a separate open issue.
    // USB phy detach REMOVED (2026-06-05). The three USB_SERIAL_JTAG_CONF0
    // register writes that used to live here (PAD_PULL_OVERRIDE set,
    // DP_PULLUP + USB_PAD_ENABLE cleared) HARD-FREEZE the chip on current
    // IDF 5.5.3 builds: the store never completes (bus stall), no panic, no
    // INT-WDT, the JTAG DTM behind the USB bridge dies too ("Unsupported DTM
    // version: -1" from OpenOCD), USB stays enumerated (pure hardware), and
    // only an EN/power reset recovers. Isolated empirically: identical
    // firmware with ONLY these three writes removed completes the whole
    // reset path (SW_CPU reboot at +1.1 s, boot frames arrive on the open
    // host fd). This also retro-explains the "10 s reset timeout" class on
    // the herdsman USB factory-reset path — the device was bricked-until-
    // replug, not slow.
    //
    // Consequence: across esp_restart() the USB-Serial-JTAG endpoint stays
    // enumerated and the host sees no port-close. zigpy-zboss handles this
    // fine (it probes the existing link after 5 s — verified live);
    // zigbee-herdsman's inReset flag again never clears on the USB
    // transport (uart.js:176), which is the pre-existing documented gap.
    // A working detach needs a sanctioned IDF mechanism (usb phy/LL API or
    // driver uninstall first) — tracked as an open follow-up.
    // 150 ms: enough for the link-layer ACK to the NCP_RESET request (sent by
    // protocol::on_rx_packet before this task was spawned) to drain out of
    // the transport buffers on either interface. The former 800 ms grace for
    // "response + host processing" is gone along with the pre-reboot
    // response itself — see cmd_handle<NCP_RESET> below.
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG,"restart");
    esp_restart();
}

// NCP_RESET deliberately sends NO pre-reboot response (changed 2026-06-05,
// pre-release hardening). The previous design replied GENERIC_OK and then
// rebooted 800 ms later — but hosts treat the NCP_RESET response as "reset
// complete" and continue IMMEDIATELY: zigbee-herdsman's driver.startup()
// does reset(FactoryReset) -> await response -> formNetwork(), and our z2m
// fork's restore-resume does reset -> backup pull. Everything they sent in
// the [response .. +800 ms] window was then killed mid-flight by our
// esp_restart (reproduced deterministically on the UART rig: the post-
// restore raw-backup pull ALWAYS died ~chunk 44, console trace
// 2026-06-05). Protocol-correct semantics instead: the RESPONSE to
// NCP_RESET is the boot-ready frame the firmware already emits after every
// boot from continue_zboss (NCP_RESET frame, tsn=0xFF) — herdsman matches
// NCP_RESET responses with a WILDCARD tsn (driver.js:235) and zigpy-zboss
// probes the link after reset rather than requiring an immediate reply
// (both verified live). So: park the erase request, reboot fast, let the
// boot-ready frame complete the host's wait — the host can only ever talk
// to a fully-rebooted, stable NCP.
template <>
struct zb_ncp::cmd_handle<NCP_RESET> {
	static void process(const zb_ncp::cmd_t& cmd, const void *buffer, size_t len) {
		uint8_t options = 0;
		if (len >= 1) {
			memcpy(&options, buffer, 1);
		}
		auto* args = new ncp_reset_args_t{options};
		BaseType_t ok = xTaskCreate(ncp_reset_deferred_task,
		                            "rst_dly", 4096, args, 5, NULL);
		if (ok != pdTRUE) {
			delete args;
			// Resource failure: reply NO_RESOURCES so the host's wait fails
			// fast instead of timing out — the device is NOT going to reboot.
			uint8_t outdata[sizeof(zb_ncp::cmd_t) + sizeof(generic_response_t)];
			auto out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
			*out_cmd = cmd;
			out_cmd->type = zb_ncp::RESPONSE;
			auto resp = reinterpret_cast<generic_response_t*>(out_cmd + 1);
			resp->category = STATUS_CATEGORY_GENERIC;
			resp->status   = GENERIC_NO_RESOURCES;
			zb_ncp::send_cmd_data(outdata, sizeof(outdata));
		}
		// Success path: no response on purpose (see block comment above).
	}
};

// Requests current Zigbee role of the local device
// [CommandId.GET_ZIGBEE_ROLE]: {
//     request: [],
//     response: [...commonResponse, {name: 'role', type: DataType.UINT8, typed: DeviceType}],
// },
template <>
struct zb_ncp::cmd_handle<GET_ZIGBEE_ROLE> : immediate_cmd_process<GET_ZIGBEE_ROLE>,
		general_status_res<GET_ZIGBEE_ROLE,uint8_t> {
	static void process_status_res(ncp_generic_status_t& status,uint8_t*  __attribute__ ((aligned(1))) res ) {
    	*res = zb_get_network_role();
    }
};

// Set Zigbee role of the local device
// [CommandId.SET_ZIGBEE_ROLE]: {
//     request: [{name: 'role', type: DataType.UINT8, typed: DeviceType}],
//     response: [...commonResponse],
// },
template <>
struct zb_ncp::cmd_handle<SET_ZIGBEE_ROLE> : immediate_cmd_process<SET_ZIGBEE_ROLE>,
		general_status_arg<SET_ZIGBEE_ROLE,uint8_t> {
	static void process_status_arg(ncp_generic_status_t& status, uint8_t role) {
    	if (role != ZB_NWK_DEVICE_TYPE_COORDINATOR) {
            status = GENERIC_INVALID_PARAMETER_1;
        }
    }
};

// Get Zigbee channels page and mask of the local device
// [CommandId.GET_ZIGBEE_CHANNEL_MASK]: {
//     request: [],
//     response: [
//         ...commonResponse,
//         {name: 'len', type: DataType.UINT8},
//         {
//             name: 'channels',
//             type: BuffaloZBOSSDataType.LIST_TYPED,
//             typed: [
//                 {name: 'page', type: DataType.UINT8},
//                 {name: 'mask', type: DataType.UINT32},
//             ],
//         },
//     ],
// },
struct GET_ZIGBEE_CHANNEL_MASK_resp_t {
	uint8_t len;
	uint8_t page;
	uint32_t mask;
} __attribute__((packed)) __attribute__ ((aligned(1)));
template <>
struct zb_ncp::cmd_handle<GET_ZIGBEE_CHANNEL_MASK> : immediate_cmd_process<GET_ZIGBEE_CHANNEL_MASK>,
		general_status_res<GET_ZIGBEE_CHANNEL_MASK,GET_ZIGBEE_CHANNEL_MASK_resp_t> {
	static void process_status_res(ncp_generic_status_t& status,GET_ZIGBEE_CHANNEL_MASK_resp_t* res) {
    	res->len = 1;
    	res->page = 0;
    	res->mask = zb_get_channel_mask();
    }
};
// // Set Zigbee channels page and mask
// [CommandId.SET_ZIGBEE_CHANNEL_MASK]: {
//     request: [
//         {name: 'page', type: DataType.UINT8},
//         {name: 'mask', type: DataType.UINT32},
//     ],
//     response: [...commonResponse],
// },
struct SET_ZIGBEE_CHANNEL_MASK_arg_t {
	uint8_t page;
	uint32_t mask;
} __attribute__((packed)) __attribute__ ((aligned(1)));
template <>
struct zb_ncp::cmd_handle<SET_ZIGBEE_CHANNEL_MASK> : immediate_cmd_process<SET_ZIGBEE_CHANNEL_MASK>,
		general_status_arg<SET_ZIGBEE_CHANNEL_MASK,SET_ZIGBEE_CHANNEL_MASK_arg_t> {
	static void process_status_arg(ncp_generic_status_t& status, const SET_ZIGBEE_CHANNEL_MASK_arg_t& arg) {
		if (arg.page != 0) {
			status = GENERIC_INVALID_PARAMETER_1;
		} else {
			zb_ncp::set_channel_mask(arg.mask);
		}
    }
};
// // Get Zigbee channel
// [CommandId.GET_ZIGBEE_CHANNEL]: {
//     request: [],
//     response: [...commonResponse, {name: 'page', type: DataType.UINT8}, {name: 'channel', type: DataType.UINT8}],
// },
struct GET_ZIGBEE_CHANNEL_resp_t {
	uint8_t page;
	uint8_t channel;
} __attribute__((packed)) __attribute__ ((aligned(1)));
template <>
struct zb_ncp::cmd_handle<GET_ZIGBEE_CHANNEL> : immediate_cmd_process<GET_ZIGBEE_CHANNEL>,
		general_status_res<GET_ZIGBEE_CHANNEL,GET_ZIGBEE_CHANNEL_resp_t> {
	static void process_status_res(ncp_generic_status_t& status,GET_ZIGBEE_CHANNEL_resp_t* res) {
    	res->page = 0;
    	res->channel = zb_get_current_channel();
    }
};
// // Requests current short PAN ID
// [CommandId.GET_PAN_ID]: {
//     request: [],
//     response: [...commonResponse, {name: 'panID', type: DataType.UINT16}],
// },
template <>
struct zb_ncp::cmd_handle<GET_PAN_ID> : immediate_cmd_process<GET_PAN_ID>,
		general_status_res<GET_PAN_ID,uint16_t> {
	static void process_status_res(ncp_generic_status_t& status,unaligned_uint16_t*  res) {
    	*res = zb_get_pan_id();
    }
};
// // Set short PAN ID
// [CommandId.SET_PAN_ID]: {
//     request: [{name: 'panID', type: DataType.UINT16}],
//     response: [...commonResponse],
// },
template <>
struct zb_ncp::cmd_handle<SET_PAN_ID> : immediate_cmd_process<SET_PAN_ID>,
		general_status_arg<SET_PAN_ID,uint16_t> {
	static void process_status_arg(ncp_generic_status_t& status, uint16_t arg) {
		// GETSET-2: 0xFFFF is the IEEE 802.15.4 unassigned/broadcast PAN ID and is
		// never a valid operating PAN. Reject it instead of forwarding a value
		// the stack can't actually run on.
		if (arg == 0xFFFF) {
			status = GENERIC_INVALID_PARAMETER_1;
			return;
		}
		// PAN-1: on a formed/joined network zb_set_pan_id() only rewrites the
		// PIB-cache mirror that zb_get_pan_id() reads — the NWK layer, the
		// on-air PAN and the NVRAM dataset all stay on the operational value.
		// The result was a getter that lied until the next reboot: the host
		// (zigpy-zboss writes its desired settings AFTER forming) saw its
		// requested PAN echoed back while the device ran — and persisted —
		// another one; after a reboot the PAN appeared to "morph" (measured:
		// runtime echo 0xF675 vs 0x32B1 on NVRAM and post-reboot; the
		// Formation.Rsp had reported 0x32B1 correctly all along). Changing
		// the 16-bit PAN of a running network is not supported by the stack,
		// so reject instead of poisoning the mirror. Pre-formation writes
		// stay allowed — ZBOSS consumes the PIB-cache value as the requested
		// PAN at formation (zigbee-herdsman sets it BEFORE NWK_FORMATION and
		// is unaffected).
		if (zb_zdo_joined()) {
			status = GENERIC_OPERATION_FAILED;
			return;
		}
		zb_set_pan_id(arg);
    }
};

 // Requests local IEEE address
// [CommandId.GET_LOCAL_IEEE_ADDR]: {
//     request: [{name: 'mac', type: DataType.UINT8}],
//     response: [...commonResponse, {name: 'mac', type: DataType.UINT8}, {name: 'ieee', type: DataType.IEEE_ADDR}],
// },
struct GET_LOCAL_IEEE_ADDR_resp_t {
	uint8_t mac;
	uint8_t ieee[8];
}__attribute__((packed)) __attribute__ ((aligned(1)));
template <>
struct zb_ncp::cmd_handle<GET_LOCAL_IEEE_ADDR> : immediate_cmd_process<GET_LOCAL_IEEE_ADDR>,
		general_status_arg_res<GET_LOCAL_IEEE_ADDR,uint8_t,GET_LOCAL_IEEE_ADDR_resp_t> {
	static void process_status_arg_res(ncp_generic_status_t& status, uint8_t arg,GET_LOCAL_IEEE_ADDR_resp_t* res ) {
		if (arg != 0) {
            ESP_LOGE(TAG,"invalid mac: %d",arg);
            status = GENERIC_INVALID_PARAMETER_1;
        } else {
            res->mac = arg;
            // issue #6: IEEE_ADDR is little-endian on the ZBOSS-NCP wire; the host
            // (zigbee-herdsman readIeeeAddr / eui64LEBufferToHex) reverses the 8
            // bytes. The old esp_read_mac(ESP_MAC_IEEE802154) returned canonical
            // MSB-first EUI-64, so the host recorded a byte-reversed *phantom*
            // coordinator IEEE and bound devices to it -> endless NWK_addr_req loop.
            // esp_zb_get_long_address() returns the OPERATIONAL long address already
            // in LE == wire order (the byte order ZBOSS uses on-air and for the backup
            // TLV) and reflects a RESTORE_NETWORK transplant (esp_zb_set_long_address).
            // Emit it verbatim.
            esp_zb_ieee_addr_t longaddr;
            esp_zb_get_long_address(longaddr);
            bool valid = false;
            for (int i = 0; i < 8; i++) { if (longaddr[i]) { valid = true; break; } }
            if (valid) {
                memcpy(res->ieee, longaddr, 8);
            } else {
                // Cold-boot fallback: long address not yet populated in the PIB.
                // efuse EUI-64 is MSB-first canonical; reverse into LE wire order.
                // For a non-restored coordinator this equals the operational addr.
                uint8_t eui[8];
                auto ret = esp_read_mac(eui,ESP_MAC_IEEE802154);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG,"failed read mac addres: %d",ret);
                    status = GENERIC_ERROR;
                } else {
                    for (int i = 0; i < 8; i++) res->ieee[i] = eui[7 - i];
                }
            }
        }
    }
};
// // Set local IEEE address
// [CommandId.SET_LOCAL_IEEE_ADDR]: {
//     request: [
//         {name: 'mac', type: DataType.UINT8},
//         {name: 'ieee', type: DataType.IEEE_ADDR},
//     ],
//     response: [...commonResponse],
// },
struct SET_LOCAL_IEEE_ADDR_arg_t {
	uint8_t mac;
	uint8_t ieee[8];
} __attribute__((packed));
template <>
struct zb_ncp::cmd_handle<SET_LOCAL_IEEE_ADDR> : immediate_cmd_process<SET_LOCAL_IEEE_ADDR>,
		general_status_arg<SET_LOCAL_IEEE_ADDR,SET_LOCAL_IEEE_ADDR_arg_t> {
	static void process_status_arg(ncp_generic_status_t& status, const SET_LOCAL_IEEE_ADDR_arg_t& arg) {
		status = GENERIC_BLOCKED;
    }
};
// // Get Transmit Power
// [CommandId.GET_TX_POWER]: {
//     request: [],
//     response: [...commonResponse, {name: 'txPower', type: DataType.UINT8}],
// },
// template <>
// struct zb_ncp::cmd_handle<GET_TX_POWER> : immediate_cmd_process<GET_TX_POWER>,
// 		general_status_res<GET_TX_POWER,uint8_t> {
// 	static void process_status_res(ncp_generic_status_t& status,uint8_t* res) {
//     	*res = zb_get_tx_power();
//     }
// };
// // Set Transmit Power
// [CommandId.SET_TX_POWER]: {
//     request: [{name: 'txPower', type: DataType.UINT8}],
//     response: [...commonResponse, {name: 'txPower', type: DataType.UINT8}],
// },
template <>
struct zb_ncp::cmd_handle<SET_TX_POWER> : immediate_cmd_process<SET_TX_POWER>,
		general_status_arg_res<SET_TX_POWER,uint8_t,uint8_t> {
	static void process_status_arg_res(ncp_generic_status_t& status, uint8_t arg,uint8_t* res) {
		// GETSET-2: the ZBOSS tx-power encoding is not reliably documented
		// (zboss_api.h labels the uint8 arg "[dBm]" yet its own example passes
		// 0x32) and the lower IEEE802154 layer is the real authority on the
		// supported range — so we do NOT impose a fabricated numeric bound.
		// Instead propagate zb_set_tx_power's own zb_ret_t: a value the stack
		// rejects now surfaces to the host instead of returning a silent OK.
		zb_ret_t ret = zb_set_tx_power(arg);
		if (ret != RET_OK) {
			status = GENERIC_INVALID_PARAMETER_1;
		}
		*res = arg;//zb_get_tx_power();
    }
};
// // Requests RxOnWhenIdle PIB attribute
// [CommandId.GET_RX_ON_WHEN_IDLE]: {
//     request: [],
//     response: [...commonResponse, {name: 'rxOn', type: DataType.UINT8}],
// },
template <>
struct zb_ncp::cmd_handle<GET_RX_ON_WHEN_IDLE> : immediate_cmd_process<GET_RX_ON_WHEN_IDLE>,
		general_status_res<GET_RX_ON_WHEN_IDLE,uint8_t> {
	static void process_status_res(ncp_generic_status_t& status, uint8_t* res) {
		*res = zb_get_rx_on_when_idle();
    }
};
// // Sets Rx On When Idle PIB attribute
// [CommandId.SET_RX_ON_WHEN_IDLE]: {
//     request: [{name: 'rxOn', type: DataType.UINT8}],
//     response: [...commonResponse],
// },
template <>
struct zb_ncp::cmd_handle<SET_RX_ON_WHEN_IDLE> : immediate_cmd_process<SET_RX_ON_WHEN_IDLE>,
		general_status_arg<SET_RX_ON_WHEN_IDLE,uint8_t> {
	static void process_status_arg(ncp_generic_status_t& status, uint8_t arg) {
		zb_set_rx_on_when_idle(arg);
    }
};
// // Requests current join status of the device
// [CommandId.GET_JOINED]: {
//     request: [],
//     response: [...commonResponse, {name: 'joined', type: DataType.UINT8}],
// },
template <>
struct zb_ncp::cmd_handle<GET_JOINED> : immediate_cmd_process<GET_JOINED>,
		general_status_res<GET_JOINED,uint8_t> {
	static void process_status_res(ncp_generic_status_t& status, uint8_t* res) {
		*res = ZB_JOINED();
    }
};

// Set NWK Key
// [CommandId.SET_NWK_KEY]: {
//     request: [
//         {name: 'nwkKey', type: DataType.SEC_KEY},
//         {name: 'index', type: DataType.UINT8},
//     ],
//     response: [...commonResponse],
// },
struct SET_NWK_KEY_arg_t {
	uint8_t nwkKey[16];
	uint8_t index;
} __attribute__((packed));
static uint8_t s_keys[3][16];
template <>
struct zb_ncp::cmd_handle<SET_NWK_KEY> : immediate_cmd_process<SET_NWK_KEY>,
		general_status_arg<SET_NWK_KEY,SET_NWK_KEY_arg_t> {
	static void process_status_arg(ncp_generic_status_t& status, const SET_NWK_KEY_arg_t& arg) {
		if (arg.index > 2) {
			status = GENERIC_INVALID_PARAMETER_2;
			return;
		}
		::memcpy(s_keys[arg.index],arg.nwkKey,16);
        zb_secur_setup_nwk_key(s_keys[arg.index],arg.index);
    }
};
// // Get list of NWK keys
// [CommandId.GET_NWK_KEYS]: {
//     request: [],
//     response: [
//         ...commonResponse,
//         {name: 'nwkKey1', type: DataType.SEC_KEY},
//         {name: 'index1', type: DataType.UINT8},
//         {name: 'nwkKey2', type: DataType.SEC_KEY},
//         {name: 'index2', type: DataType.UINT8},
//         {name: 'nwkKey3', type: DataType.SEC_KEY},
//         {name: 'index3', type: DataType.UINT8},
//     ],
// },
struct GET_NWK_KEYS_resp_t {
	uint8_t nwkKey1[16];
	uint8_t index1;
	uint8_t nwkKey2[16];
	uint8_t index2;
	uint8_t nwkKey3[16];
	uint8_t index3;
} __attribute__((packed));
// esp-zboss-lib exposes no public getter for the active network key, and for an
// NCP the host-side key is pure bookkeeping: the stack does all crypto itself,
// so the running network is unaffected by what we report here. The only consumer
// is zigpy-zboss' load_network_info(), which since v2.0.2 hard-aborts the whole
// connect when GET_NWK_KEYS returns a blank (all-0x00/all-0xFF) key — that abort
// is the bug this handler fixes. We deliberately return a fixed, recognizable
// placeholder rather than the real key:
//   * it clears zigpy's blank-key guard, so resume/connect succeeds, and
//   * being obviously not a real key, it does not pretend the resulting ZHA
//     network backup is restorable — which it is not against this firmware: the
//     restore path (write_network_info) needs NVRAM_WRITE(ZB_IB_COUNTERS) to
//     restore the NWK frame counter, which we do not implement, so devices have
//     to be re-paired regardless of the key.
// herdsman's zboss adapter never queries this command (supportsBackup()=false);
// real backup/restore goes through GET_NETWORK_BACKUP (0x99) / RESTORE_NETWORK
// (0x9A) — a full NVRAM snapshot that already contains the real key.
template <>
struct zb_ncp::cmd_handle<GET_NWK_KEYS> : immediate_cmd_process<GET_NWK_KEYS>,
		general_status_res<GET_NWK_KEYS,GET_NWK_KEYS_resp_t> {
	static void process_status_res(ncp_generic_status_t& status, GET_NWK_KEYS_resp_t* res) {
		(void)status; // left at GENERIC_OK by the general_status_res mixin
		static const uint8_t placeholder[16] = {
			'E','S','P','-','Z','B','O','S','S','-','N','O','-','K','E','Y'
		};
		::memset(res, 0, sizeof(*res));
		::memcpy(res->nwkKey1, placeholder, sizeof(placeholder));
		res->index1 = 0; // key2/key3 slots stay all-zero, index2/index3 = 0
	}
};

// Get Extended Pan ID
// [CommandId.GET_EXTENDED_PAN_ID]: {
//     request: [],
//     response: [...commonResponse, {name: 'extendedPanID', type: BuffaloZBOSSDataType.EXTENDED_PAN_ID}],
// },
template <>
struct zb_ncp::cmd_handle<GET_EXTENDED_PAN_ID> : immediate_cmd_process<GET_EXTENDED_PAN_ID>,
		general_status_res<GET_EXTENDED_PAN_ID,zb_ext_pan_id_t> {
	static void process_status_res(ncp_generic_status_t& status, zb_ext_pan_id_t* res) {
		zb_uint8_t ext_pan_id[8];
		zb_get_extended_pan_id(ext_pan_id);

        // GETSET-3: returned reversed (MSB-first) to match the reference ZBOSS NCP
        // + herdsman's compensating .reverse(); SET_EXTENDED_PAN_ID mirrors this
        // so the SET<->GET pair round-trips.
        zb_uint8_t ext_pan_id_reversed[8];
        ext_pan_id_reversed[0] = ext_pan_id[7];
        ext_pan_id_reversed[1] = ext_pan_id[6];
        ext_pan_id_reversed[2] = ext_pan_id[5];
        ext_pan_id_reversed[3] = ext_pan_id[4];
        ext_pan_id_reversed[4] = ext_pan_id[3];
        ext_pan_id_reversed[5] = ext_pan_id[2];
        ext_pan_id_reversed[6] = ext_pan_id[1];
        ext_pan_id_reversed[7] = ext_pan_id[0];

        memcpy(*res, ext_pan_id_reversed, 8);
    }
};

// Get Coordinator version
// [CommandId.GET_COORDINATOR_VERSION]: {
//     request: [],
//     response: [...commonResponse, {name: 'version', type: DataType.UINT8}],
// },
extern "C" uint8_t zb_aib_get_coordinator_version();
template <>
struct zb_ncp::cmd_handle<GET_COORDINATOR_VERSION> : immediate_cmd_process<GET_COORDINATOR_VERSION>,
		general_status_res<GET_COORDINATOR_VERSION,uint8_t> {
	static void process_status_res(ncp_generic_status_t& status, uint8_t* res) {
		*res = zb_aib_get_coordinator_version();
    }
};


// Get own short address. Needed by zigpy-zboss's ControllerApplication.
// load_network_info() — without this, NWKAddr stays None and start_network's
// ZbossCoordinator construction raises TypeError(int(None)).
// [CommandId.GET_SHORT_ADDRESS]: {
//     request: [],
//     response: [...commonResponse, {name: 'nwk', type: DataType.UINT16}],
// },
template <>
struct zb_ncp::cmd_handle<GET_SHORT_ADDRESS> : immediate_cmd_process<GET_SHORT_ADDRESS>,
		general_status_res<GET_SHORT_ADDRESS,uint16_t> {
	// res points into the packed FullRes wire buffer, so it must be the
	// byte-aligned typedef — a plain uint16_t* trips -Werror=address-of-packed-member
	// under stricter toolchains (e.g. IDF 6.x / newer GCC). Mirrors GET_PAN_ID.
	static void process_status_res(ncp_generic_status_t& status, unaligned_uint16_t* res) {
		*res = zb_get_short_address();
    }
};


// Sets TC Policy
// [CommandId.SET_TC_POLICY]: {
//     request: [
//         {name: 'type', type: DataType.UINT16, typed: PolicyType},
//         {name: 'value', type: DataType.UINT8},
//     ],
//     response: [...commonResponse],
// },
struct SET_TC_POLICY_arg_t {
	uint16_t type;
	uint8_t value;
} __attribute__((packed));
enum PolicyType {
    LINK_KEY_REQUIRED = 0,
    IC_REQUIRED = 1,
    TC_REJOIN_ENABLED = 2,
    IGNORE_TC_REJOIN = 3,
    APS_INSECURE_JOIN = 4,
    DISABLE_NWK_MGMT_CHANNEL_UPDATE = 5,
};
extern "C" void zb_aib_tcpol_set_update_trust_center_link_keys_required(uint8_t);
template <>
struct zb_ncp::cmd_handle<SET_TC_POLICY> : immediate_cmd_process<SET_TC_POLICY>,
		general_status_arg<SET_TC_POLICY,SET_TC_POLICY_arg_t> {
	static void process_status_arg(ncp_generic_status_t& status, const SET_TC_POLICY_arg_t& arg) {
		switch(arg.type) {
		case LINK_KEY_REQUIRED:
			zb_aib_tcpol_set_update_trust_center_link_keys_required(arg.value);
			break;
		case IC_REQUIRED:
			zb_set_installcode_policy(arg.value);
			break;
		case TC_REJOIN_ENABLED:
			zb_secur_set_tc_rejoin_enabled(arg.value);
			break;
		case IGNORE_TC_REJOIN:
			zb_secur_set_ignore_tc_rejoin(arg.value);
			break;
		case APS_INSECURE_JOIN:
			zb_zdo_set_aps_unsecure_join(arg.value);
			break;
		case DISABLE_NWK_MGMT_CHANNEL_UPDATE:
			zb_zdo_disable_network_mgmt_channel_update(arg.value);
			break;
		default:
			status = GENERIC_INVALID_PARAMETER_1;
		};
    }
};
// // Sets an extended PAN ID
// [CommandId.SET_EXTENDED_PAN_ID]: {
//     request: [{name: 'extendedPanID', type: BuffaloZBOSSDataType.EXTENDED_PAN_ID}],
//     response: [...commonResponse],
// },
template <>
struct zb_ncp::cmd_handle<SET_EXTENDED_PAN_ID> : immediate_cmd_process<SET_EXTENDED_PAN_ID>,
		general_status_arg<SET_EXTENDED_PAN_ID,zb_ext_pan_id_t> {
	static void process_status_arg(ncp_generic_status_t& status, const zb_ext_pan_id_t& arg) {
		// GETSET-3 (resolved 2026-05-30): mirror GET_EXTENDED_PAN_ID's byte
		// reversal so SET<->GET round-trips identically (stack = reverse(wire);
		// GET then returns reverse(stack) == wire). GET's reversal matches the
		// reference ZBOSS NCP, which zigbee-herdsman compensates for with its own
		// .reverse() (driver.ts:174); herdsman keeps SET_EXTENDED_PAN_ID commented
		// out (driver.ts:224), so this only affects a host that actually issues
		// SET (e.g. a future zigpy-zboss / ZHA path). NOTE: no current host
		// exercises the firmware SET path — needs a hardware SET->GET round-trip
		// check before release.
		zb_uint8_t ext_pan_id_reversed[8];
		ext_pan_id_reversed[0] = arg[7];
		ext_pan_id_reversed[1] = arg[6];
		ext_pan_id_reversed[2] = arg[5];
		ext_pan_id_reversed[3] = arg[4];
		ext_pan_id_reversed[4] = arg[3];
		ext_pan_id_reversed[5] = arg[2];
		ext_pan_id_reversed[6] = arg[1];
		ext_pan_id_reversed[7] = arg[0];
		zb_set_extended_pan_id(ext_pan_id_reversed);
    }
};

// // Sets the maximum number of children
// [CommandId.SET_MAX_CHILDREN]: {
//     request: [{name: 'children', type: DataType.UINT8}],
//     response: [...commonResponse],
// },
template <>
struct zb_ncp::cmd_handle<SET_MAX_CHILDREN> : immediate_cmd_process<SET_MAX_CHILDREN>,
		general_status_arg<SET_MAX_CHILDREN,uint8_t> {
	static void process_status_arg(ncp_generic_status_t& status, uint8_t arg) {
		// GETSET-2: the address/child tables are sized for
		// zb_ncp::OVERALL_NETWORK_SIZE nodes (init_int). A host value above that
		// can't be honored and oversizing children risks the zb_address.c:395
		// table-exhaustion assertion family — reject it.
		if (arg > zb_ncp::OVERALL_NETWORK_SIZE) {
			status = GENERIC_INVALID_PARAMETER_1;
			return;
		}
		zb_set_max_children(arg);
    }
};

// // Add or update Simple descriptor for a specified endpoint
// [CommandId.AF_SET_SIMPLE_DESC]: {
//     request: [
//         {name: 'endpoint', type: DataType.UINT8},
//         {name: 'profileID', type: DataType.UINT16},
//         {name: 'deviceID', type: DataType.UINT16},
//         {name: 'version', type: DataType.UINT8},
//         {name: 'inputClusterCount', type: DataType.UINT8},
//         {name: 'outputClusterCount', type: DataType.UINT8},
//         {
//             name: 'inputClusters',
//             type: BuffaloZclDataType.LIST_UINT16,
//             options: (payload, options) => (options.length = payload.inputClusterCount),
//         },
//         {
//             name: 'outputClusters',
//             type: BuffaloZclDataType.LIST_UINT16,
//             options: (payload, options) => (options.length = payload.outputClusterCount),
//         },
//     ],
//     response: [...commonResponse],
// },
struct AF_SET_SIMPLE_DESC_arg_hdr_t {
	uint8_t endpoint;
	uint16_t profileID;
	uint16_t deviceID;
	uint8_t version;
	uint8_t inputClusterCount;
	uint8_t outputClusterCount;
} __attribute__((packed));

extern "C" zb_ret_t zb_add_simple_descriptor(const void*simple_desc);
template <>
struct zb_ncp::cmd_handle<AF_SET_SIMPLE_DESC> : immediate_cmd_process<AF_SET_SIMPLE_DESC> {
	static constexpr size_t resp_buffer_size = 2;
	static size_t process_immediate(const void *inbuffer, size_t inlen,uint8_t* outdata,size_t outdata_size) {
		auto full_res = reinterpret_cast<generic_response_t*>(outdata);
		full_res->category = STATUS_CATEGORY_GENERIC;
		full_res->status = GENERIC_OK;
		if (inlen < sizeof(AF_SET_SIMPLE_DESC_arg_hdr_t)) {
			full_res->status = GENERIC_INVALID_PARAMETER;
			return sizeof(generic_response_t);
		}
		auto arg = static_cast<const AF_SET_SIMPLE_DESC_arg_hdr_t*>(inbuffer);
		ESP_LOGI(TAG,"AF_SET_SIMPLE_DESC %d, Skip",int(arg->endpoint));
		//zb_add_simple_descriptor(arg);
		return sizeof(generic_response_t);
    }
};

// // NLME-NETWORK-FORMATION.request
// [CommandId.NWK_FORMATION]: {
//     request: [
//         {name: 'len', type: DataType.UINT8},
//         {
//             name: 'channels',
//             type: BuffaloZBOSSDataType.LIST_TYPED,
//             typed: [
//                 {name: 'page', type: DataType.UINT8},
//                 {name: 'mask', type: DataType.UINT32},
//             ],
//         },
//         {name: 'duration', type: DataType.UINT8},
//         {name: 'distribFlag', type: DataType.UINT8},
//         {name: 'distribNwk', type: DataType.UINT16},
//         {name: 'extendedPanID', type: BuffaloZBOSSDataType.EXTENDED_PAN_ID},
//     ],
//     response: [...commonResponse, {name: 'nwk', type: DataType.UINT16}],
// },

template<>
struct zb_ncp::cmd_handle<NWK_FORMATION> : delayed_cmd_process<NWK_FORMATION,single_cmd_delayed> {
    static constexpr size_t resp_buffer_size = 8;
    static constexpr const char* name = "NWK_FORMATION";
    //static uint32_t channel_mask;
    static int start_delayed(const void* cmddata,uint16_t cmdlen) {
        if (cmdlen < 1)
            return GENERIC_INVALID_PARAMETER;
        const uint8_t* indata = static_cast<const uint8_t*>(cmddata);
        uint8_t len = *indata++; --cmdlen;
        for (size_t i=0;i<len;++i) {
            if (cmdlen < 5) {
                return GENERIC_INVALID_PARAMETER;
            }
            uint8_t page = *indata++; --cmdlen;
            if (page != 0) 
                return GENERIC_INVALID_PARAMETER;
            uint32_t channel_mask; memcpy(&channel_mask, indata, 4);
            indata+=4; cmdlen-=4;
            ESP_LOGD(TAG,"zb_set_channel_mask: %08lx",channel_mask);
            zb_ncp::set_channel_mask(channel_mask);
        }
        if (cmdlen < 1)
            return GENERIC_INVALID_PARAMETER;
        uint8_t duration = *indata++; --cmdlen;
        if (cmdlen < 1)
            return GENERIC_INVALID_PARAMETER;
        uint8_t distribFlag = *indata++; --cmdlen;
        if (cmdlen < 2)
            return GENERIC_INVALID_PARAMETER;
        uint16_t distribNwk; memcpy(&distribNwk, indata, 2);
        indata+=2;cmdlen-=2;
        if (cmdlen < sizeof(zb_ext_pan_id_t))
            return GENERIC_INVALID_PARAMETER;
        const zb_ext_pan_id_t* extendedPanID = reinterpret_cast<const zb_ext_pan_id_t*>(indata);
        zb_set_extended_pan_id(*extendedPanID);

        (void)duration;
        (void)distribNwk;
        (void)distribFlag;

        zb_ext_pan_id_t ext_pan_id;
        zb_get_extended_pan_id(ext_pan_id);
        ESP_LOGD(TAG,"set ext pan_id: %02x%02x%02x%02x%02x%02x%02x%02x / %02x%02x%02x%02x%02x%02x%02x%02x",
            int((*extendedPanID)[0]),int((*extendedPanID)[1]),int((*extendedPanID)[2]),int((*extendedPanID)[3]),int((*extendedPanID)[4]),int((*extendedPanID)[5]),int((*extendedPanID)[6]),int((*extendedPanID)[7]),
            int(ext_pan_id[0]),int(ext_pan_id[1]),int(ext_pan_id[2]),int(ext_pan_id[3]),int(ext_pan_id[4]),int(ext_pan_id[5]),int(ext_pan_id[6]),int(ext_pan_id[7]));
        

        if (!zb_ncp::start_zigbee_stack()) {

            if (!bdb_start_top_level_commissioning(ZB_BDB_NETWORK_FORMATION)) {
                return GENERIC_ERROR;
            }
        }
        return 0;
    }
    static uint16_t finish_delayed(int status,uint8_t* outdata,uint16_t outlen) {
        *outdata++ = STATUS_CATEGORY_NWK;
        *outdata++ = status;
        // ZBOSS NCP spec formation response body: NWKAddr + PANID + Page +
        // Channel. The pre-2026-06-05 version sent ONLY zb_get_pan_id() in
        // the first (NWKAddr) slot — zigpy-zboss <=1.2.0 and zigbee-herdsman
        // only parse that first u16 (herdsman names it 'nwk'), so the bug was
        // invisible; zigpy-zboss PR #73 (zigpy 0.92+ support) parses the full
        // spec layout and its Formation waiter times out on the short frame.
        // Trailing extra bytes are ignored by both old parsers, so emitting
        // the full layout is backward-compatible.
        uint16_t nwk_addr_ = 0; uint16_t pan_id_ = 0; uint8_t channel_ = 0;
        if (status == 0) {
            nwk_addr_ = zb_get_short_address();
            pan_id_   = zb_get_pan_id();
            channel_  = zb_get_current_channel();
        }
        memcpy(outdata, &nwk_addr_, 2); outdata += 2;
        memcpy(outdata, &pan_id_, 2);  outdata += 2;
        *outdata++ = 0;        // channel page (2.4 GHz)
        *outdata++ = channel_;
        return 8;
    }
};
SINGLE_CMD_DELAYED_DECL(NWK_FORMATION)


    // // NLME-PERMIT-JOINING.request
    // [CommandId.NWK_PERMIT_JOINING]: {
    //     request: [{name: 'duration', type: DataType.UINT8}],
    //     response: [...commonResponse],
    // },
template <>
struct zb_ncp::cmd_handle<NWK_PERMIT_JOINING> : immediate_cmd_process<NWK_PERMIT_JOINING>,
		general_status_arg<NWK_PERMIT_JOINING,uint8_t> {
		static void process_status_arg(ncp_generic_status_t& status, uint8_t duration) {
        zb_bufid_t buf = zb_buf_get_out();
        if (!buf) {
            status = GENERIC_NO_RESOURCES;
            return;
        }
        zb_zdo_mgmt_permit_joining_req_param_t *req = ZB_BUF_GET_PARAM(buf, zb_zdo_mgmt_permit_joining_req_param_t);
        req->dest_addr = 0x0000; // send to self
        req->permit_duration = duration;
        req->tc_significance = 1;
        zb_zdo_mgmt_permit_joining_req(buf, [](uint8_t buf){ zb_buf_free(buf); });
    }
};

// // Start without forming a new network.
// [CommandId.NWK_START_WITHOUT_FORMATION]: {
//     request: [],
//     response: [...commonResponse],
// },

template<>
struct zb_ncp::cmd_handle<NWK_START_WITHOUT_FORMATION> : delayed_cmd_process<NWK_START_WITHOUT_FORMATION,single_cmd_delayed> {
    static constexpr size_t resp_buffer_size = 2;
    static constexpr const char* name = "NWK_START_WITHOUT_FORMATION";
    //static uint32_t channel_mask;
    static int start_delayed(const void* cmddata,uint16_t cmdlen) {
        if (!zb_ncp::start_zigbee_stack()) {
            response(0);
        }
        return 0;
    }
    static uint16_t finish_delayed(int status,uint8_t* outdata,uint16_t outlen) {
        *outdata++ = STATUS_CATEGORY_NWK;
        *outdata++ = status;
        return 2;
    }
};
SINGLE_CMD_DELAYED_DECL(NWK_START_WITHOUT_FORMATION)


// // [ZdoClusterId.ROUTING_TABLE_REQUEST]: CommandId.ZDO_MGMT_RTG_REQ,


// Get a list of Active Endpoints from a remote device
// [CommandId.ZDO_ACTIVE_EP_REQ]: {
//     request: [{name: 'nwk', type: DataType.UINT16}],
//     response: [
//         ...commonResponse,
//         {name: 'len', type: DataType.UINT8},
//         {name: 'endpoints', type: BuffaloZclDataType.LIST_UINT8, options: (payload, options) => (options.length = payload.len)},
//         {name: 'nwk', type: DataType.UINT16},
//     ],
// },


template<>
struct zb_ncp::cmd_handle<ZDO_ACTIVE_EP_REQ> : request_cmd_process< ZDO_ACTIVE_EP_REQ, uint16_t, zb_zdo_active_ep_req_t, zb_zdo_ep_resp_t > {
    using Base = request_cmd_process< ZDO_ACTIVE_EP_REQ, uint16_t, zb_zdo_active_ep_req_t, zb_zdo_ep_resp_t >;
    static constexpr size_t additional_buffer_size = 16;
    static constexpr const char* name = "ZDO_ACTIVE_EP_REQ";
    static uint8_t start_request(uint8_t buf) {
        return zb_zdo_active_ep_req(buf,&Base::req_cb);
    }
    static void format_request(zb_zdo_active_ep_req_t& req,uint16_t arg) {
    	ESP_LOGI(TAG,"ZDO_ACTIVE_EP_REQ::start_request nwk_addr:%04x",arg);
        req.nwk_addr = arg;
    }
    static uint16_t format_response(uint8_t* outdata,const zb_zdo_ep_resp_t* resp) {
        auto len = Base::format_response(outdata,resp);
        uint16_t ep_count = resp->ep_count;
        if (ep_count > 16) {
            ESP_LOGE(TAG,"S_ZDO_ACTIVE_EP_REQ: truncate endpoints %d",int(ep_count));
            ep_count = 16;
        }
        outdata[2] = ep_count;
        const uint8_t* ep_list = reinterpret_cast<const uint8_t*>(resp+1);
        ESP_LOGI(TAG,"S_ZDO_ACTIVE_EP_REQ::format_response nwk_addr:%04x %d",resp->nwk_addr,ep_count);
        for (uint16_t i=0;i<ep_count;++i) {
            outdata[3+i] = ep_list[i]; ++len;
            ESP_LOGI(TAG,"S_ZDO_ACTIVE_EP_REQ::format_response ep:%d",ep_list[i]);
        }
        memcpy(&outdata[3+ep_count], &resp->nwk_addr, 2);
        return 3+ep_count+2;
    }
};
//REQUEST_CMD_PROCESS_DECL(ZDO_ACTIVE_EP_REQ,uint16_t)

 

// Get the Simple Descriptor from a remote device
    // [CommandId.ZDO_SIMPLE_DESC_REQ]: {
    //     request: [
    //         {name: 'nwk', type: DataType.UINT16},
    //         {name: 'endpoint', type: DataType.UINT8},
    //     ],
    //     response: [
    //         ...commonResponse,
    //         {name: 'endpoint', type: DataType.UINT8},
    //         {name: 'profileID', type: DataType.UINT16},
    //         {name: 'deviceID', type: DataType.UINT16},
    //         {name: 'version', type: DataType.UINT8},
    //         {name: 'inputClusterCount', type: DataType.UINT8},
    //         {name: 'outputClusterCount', type: DataType.UINT8},
    //         {
    //             name: 'inputClusters',
    //             type: BuffaloZclDataType.LIST_UINT16,
    //             options: (payload, options) => (options.length = payload.inputClusterCount),
    //         },
    //         {
    //             name: 'outputClusters',
    //             type: BuffaloZclDataType.LIST_UINT16,
    //             options: (payload, options) => (options.length = payload.outputClusterCount),
    //         },
    //         {name: 'nwk', type: DataType.UINT16},
    //     ],
    // },

// /** @brief Header of simple_desc_resp primitive.  */
// typedef ZB_PACKED_PRE struct zb_zdo_simple_desc_resp_hdr_s
// {
//   zb_uint8_t      tsn; /*!< ZDP transaction sequence number */
//   zb_uint8_t      status;   /*!< The status of the Desc_req command. @ref zdp_status */
//   zb_uint16_t     nwk_addr; /*!< NWK address for the request  */
//   zb_uint8_t      length;   /*!< Length of the simple descriptor */
// } ZB_PACKED_STRUCT
// zb_zdo_simple_desc_resp_hdr_t;

template<>
struct zb_ncp::cmd_handle<ZDO_SIMPLE_DESC_REQ> : request_cmd_process< ZDO_SIMPLE_DESC_REQ, zb_zdo_simple_desc_req_t, zb_zdo_simple_desc_req_t, zb_zdo_simple_desc_resp_t > {
	 using Base = request_cmd_process< ZDO_SIMPLE_DESC_REQ, zb_zdo_simple_desc_req_t, zb_zdo_simple_desc_req_t, zb_zdo_simple_desc_resp_t >;
    static constexpr size_t additional_buffer_size = 2*32;
    static constexpr const char* name = "ZDO_SIMPLE_DESC_REQ";
    static uint8_t start_request(uint8_t buf) {
        return zb_zdo_simple_desc_req(buf,&Base::req_cb);
    }
    static uint16_t format_response(uint8_t* outdata,const zb_zdo_simple_desc_resp_t* resp) {
    	auto out = outdata;
    	*out++ = STATUS_CATEGORY_ZDO;
    	*out++ = resp->hdr.status;
    	*out++ = resp->simple_desc.endpoint;
    	memcpy(out, &resp->simple_desc.app_profile_id, 2); out+=2;
    	memcpy(out, &resp->simple_desc.app_device_id, 2); out+=2;
    	*out++ = resp->simple_desc.app_device_version;

    	auto input_count = resp->simple_desc.app_input_cluster_count;
        auto output_count = resp->simple_desc.app_output_cluster_count;
        while ((input_count+output_count) > 32) {
            if (input_count > output_count) --input_count;
            else --output_count;
        }

    	*out++ = input_count;
    	*out++ = output_count;

    	for (uint16_t i=0;i<input_count;++i) {
    		memcpy(out, &resp->simple_desc.app_cluster_list[i], 2);
    		out+=2;
    	}
    	for (uint16_t i=0;i<output_count;++i) {
    		memcpy(out, &resp->simple_desc.app_cluster_list[i+resp->simple_desc.app_input_cluster_count], 2);
    		out+=2;
    	}
    	memcpy(out, &resp->hdr.nwk_addr, 2); out+=2;

        return out-outdata;
    }
};
//REQUEST_CMD_PROCESS_DECL(ZDO_SIMPLE_DESC_REQ,zb_zdo_simple_desc_req_t)

// Get the Node Descriptor from a remote device
// [CommandId.ZDO_NODE_DESC_REQ]: {
//     request: [{name: 'nwk', type: DataType.UINT16}],
//     response: [
//         ...commonResponse,
//         {name: 'flags', type: DataType.UINT16},
//         {name: 'macCapabilities', type: DataType.UINT8},
//         {name: 'manufacturerCode', type: DataType.UINT16},
//         {name: 'bufferSize', type: DataType.UINT8},
//         {name: 'incomingSize', type: DataType.UINT16},
//         {name: 'serverMask', type: DataType.UINT16},
//         {name: 'outgoingSize', type: DataType.UINT16},
//         {name: 'descriptorCapabilities', type: DataType.UINT8},
//         {name: 'nwk', type: DataType.UINT16},
//     ],
// },

// typedef ZB_PACKED_PRE struct zb_af_node_desc_s
// {
//   zb_uint16_t          node_desc_flags;            /*!< node description */
//   zb_uint8_t           mac_capability_flags;       /*!< mac capability */
//   zb_uint16_t          manufacturer_code;          /*!< Manufacturer code */
//   zb_uint8_t           max_buf_size;               /*!< Maximum buffer size */
//   zb_uint16_t          max_incoming_transfer_size; /*!< Maximum incoming transfer size */
//   zb_uint16_t          server_mask;                /*!< Server mask */
//   zb_uint16_t          max_outgoing_transfer_size; /*!< Maximum outgoing transfer size */
//   zb_uint8_t           desc_capability_field;      /*!< Descriptor capability field */
// }
// ZB_PACKED_STRUCT
// zb_af_node_desc_t;

template<>
struct zb_ncp::cmd_handle<ZDO_NODE_DESC_REQ> : request_cmd_process< ZDO_NODE_DESC_REQ, zb_zdo_node_desc_req_t, zb_zdo_node_desc_req_t, zb_zdo_node_desc_resp_t > {
    using Base = request_cmd_process< ZDO_NODE_DESC_REQ, zb_zdo_node_desc_req_t, zb_zdo_node_desc_req_t, zb_zdo_node_desc_resp_t >;
    static constexpr size_t additional_buffer_size = 0;
    static constexpr const char* name = "ZDO_NODE_DESC_REQ";
    static void format_request(zb_zdo_node_desc_req_t& req,const zb_zdo_node_desc_req_t& arg) {
    	req = arg;
    	ESP_LOGI(TAG,"ZDO_NODE_DESC_REQ %04x",req.nwk_addr);
    }
    static uint8_t start_request(uint8_t buf) {
        return zb_zdo_node_desc_req(buf,&Base::req_cb);
    }
    static uint16_t format_response(uint8_t* outdata,const zb_zdo_node_desc_resp_t* resp) {
        outdata[0] = STATUS_CATEGORY_ZDO;
        outdata[1] = resp->hdr.status;
        memcpy(&outdata[2],&resp->node_desc,sizeof(zb_af_node_desc_t));
        memcpy(&outdata[2+sizeof(zb_af_node_desc_t)], &resp->hdr.nwk_addr, 2);
        return sizeof(zb_zdo_node_desc_resp_t);
    }
};
//REQUEST_CMD_PROCESS_DECL(ZDO_NODE_DESC_REQ,zb_zdo_node_desc_req_t)



// Request a remote device or devices to allow or disallow association
// [CommandId.ZDO_PERMIT_JOINING_REQ]: {
//     request: [
//         {name: 'nwk', type: DataType.UINT16},
//         {name: 'duration', type: DataType.UINT8},
//         {name: 'tcSignificance', type: DataType.UINT8},
//     ],
//     response: [...commonResponse],
// },
template<>
struct zb_ncp::cmd_handle<ZDO_PERMIT_JOINING_REQ> : request_cmd_process< ZDO_PERMIT_JOINING_REQ, zb_zdo_mgmt_permit_joining_req_param_t, zb_zdo_mgmt_permit_joining_req_param_t, zb_zdo_mgmt_permit_joining_resp_t > {
    using Base = request_cmd_process< ZDO_PERMIT_JOINING_REQ, zb_zdo_mgmt_permit_joining_req_param_t, zb_zdo_mgmt_permit_joining_req_param_t, zb_zdo_mgmt_permit_joining_resp_t >;
    static constexpr size_t additional_buffer_size = 0;
    static constexpr bool request_is_data = false;
    static constexpr const char* name = "ZDO_PERMIT_JOINING_REQ";
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_PERMIT_JOINING_REQ::start_request nwk_addr:%04x time:%d",s_req.dest_addr,int(s_req.permit_duration));
        return zb_zdo_mgmt_permit_joining_req(buf,&Base::req_cb);
    }

    // A ZDO broadcast is never answered (Zigbee spec 2.4.3.3.7; zigbee-herdsman
    // sends exactly this one with disableResponse=true), so no Mgmt_Permit_Joining_rsp
    // can ever arrive — yet our request callback still fires. ZBOSS hands it the
    // REQUEST buffer back, and zdo_send_req() has prepended the ZDP TSN to that
    // buffer (zb_buf_alloc_left(param,1)), so it reads
    //     [tsn][permit_duration][tc_significance]
    // which parsed as zb_zdo_mgmt_permit_joining_resp_t is {tsn, status=duration}.
    // The TSN therefore matches (the slot resolves) while the "status" is just an
    // echo of the duration we sent — not a ZDP status at all. That is where the
    // host-visible nonsense came from: z2m opens with duration=254, so herdsman
    // logged `PERMIT_JOINING_RESPONSE, status=undefined` (254 is no ZDO status)
    // and diagnostics chased a phantom error. Nothing had failed.
    //
    // Mechanism read off libzboss_stack.zczr (esp-zboss-lib 1.6.4,
    // zdo_nwk_manage_cli.c / zdo_common.c); bench-proven 6/6 with
    // test/permit_bcast_status_probe.py — the value tracks the duration byte,
    // including ZBOSS clamping duration 255 to 254.
    //
    // Report success instead: the broadcast itself went out, which is all a
    // broadcast can tell us. Guarded twice so nothing real gets masked — only for
    // broadcast destinations (>= 0xFFF8, the same test the stack itself uses to
    // classify the address) and only when the byte really is that echo, so a
    // future stack that delivers a genuine status still reaches the host verbatim.
    static bool response_body_is_synthetic(const request_t& req, uint8_t& status) {
        if (req.arg.dest_addr < 0xFFF8) {
            return false;               // unicast, incl. self 0x0000: real response
        }
        const uint8_t echo = (req.arg.permit_duration == 0xFF) ? 0xFE
                                                              : req.arg.permit_duration;
        if (status != echo) {
            return false;               // not the echo — pass it through untouched
        }
        status = 0;
        return true;
    }
};

 // Send Bind request to a remote device
// [CommandId.ZDO_BIND_REQ]: {
//     request: [
//         {name: 'target', type: DataType.UINT16},
//         {name: 'srcIeee', type: DataType.IEEE_ADDR},
//         {name: 'srcEP', type: DataType.UINT8},
//         {name: 'clusterID', type: DataType.UINT16},
//         {name: 'addrMode', type: DataType.UINT8},
//         {name: 'dstIeee', type: DataType.IEEE_ADDR},
//         {name: 'dstEP', type: DataType.UINT8},
//     ],
//     response: [...commonResponse],
// },

struct S_ZDO_BIND_REQ_arg_t {
    uint16_t target;
    zb_ieee_addr_t srcIeee;
    uint8_t srcEP;
    uint16_t clusterID;
    uint8_t addrMode;
    zb_ieee_addr_t dstIeee;
    uint8_t dstEP;
} __attribute__((packed));


template<>
struct zb_ncp::cmd_handle<ZDO_BIND_REQ> : request_cmd_process< ZDO_BIND_REQ, S_ZDO_BIND_REQ_arg_t, zb_zdo_bind_req_param_t, zb_zdo_bind_resp_t > {
    using Base = request_cmd_process< ZDO_BIND_REQ, S_ZDO_BIND_REQ_arg_t, zb_zdo_bind_req_param_t, zb_zdo_bind_resp_t >;
    static constexpr size_t additional_buffer_size = 0;
    static constexpr const char* name = "ZDO_BIND_REQ";
    static constexpr bool request_is_data = false;
    static void format_request(zb_zdo_bind_req_param_t& req,const S_ZDO_BIND_REQ_arg_t& arg) {
    	memcpy(req.src_address,arg.srcIeee,8);
    	req.src_endp = arg.srcEP;
    	req.cluster_id = arg.clusterID;
    	req.dst_addr_mode = arg.addrMode;
    	memcpy(&req.dst_address,&arg.dstIeee,8);
    	req.dst_endp = arg.dstEP;
    	req.req_dst_addr = arg.target;
    }
    static uint8_t start_request(uint8_t buf) {
        return zb_zdo_bind_req(buf,&Base::req_cb);
    }
};

using S_ZDO_UNBIND_REQ_arg_t = S_ZDO_BIND_REQ_arg_t;

template<>
struct zb_ncp::cmd_handle<ZDO_UNBIND_REQ> : request_cmd_process< ZDO_UNBIND_REQ, S_ZDO_UNBIND_REQ_arg_t, zb_zdo_bind_req_param_t, zb_zdo_bind_resp_t > {
    using Base = request_cmd_process< ZDO_UNBIND_REQ, S_ZDO_UNBIND_REQ_arg_t, zb_zdo_bind_req_param_t, zb_zdo_bind_resp_t >;
    static constexpr size_t additional_buffer_size = 0;
    static constexpr const char* name = "ZDO_UNBIND_REQ";
    static constexpr bool request_is_data = false;
    static void format_request(zb_zdo_bind_req_param_t& req,const S_ZDO_UNBIND_REQ_arg_t& arg) {
    	memcpy(req.src_address,arg.srcIeee,8);
    	req.src_endp = arg.srcEP;
    	req.cluster_id = arg.clusterID;
    	req.dst_addr_mode = arg.addrMode;
    	memcpy(&req.dst_address,&arg.dstIeee,8);
    	req.dst_endp = arg.dstEP;
    	req.req_dst_addr = arg.target;
    }
    static uint8_t start_request(uint8_t buf) {
        return zb_zdo_unbind_req(buf,&Base::req_cb);
    }
};


// Request for a remote device IEEE address
// [CommandId.ZDO_IEEE_ADDR_REQ]: {
//     request: [
//         {name: 'destNwk', type: DataType.UINT16},
//         {name: 'nwk', type: DataType.UINT16},
//         {name: 'type', type: DataType.UINT8},
//         {name: 'startIndex', type: DataType.UINT8},
//     ],
//     response: [
//         ...commonResponse,
//         {name: 'ieee', type: DataType.IEEE_ADDR},
//         {name: 'nwk', type: DataType.UINT16},
//         {name: 'num', type: DataType.UINT8, condition: (payload, buffalo) => buffalo && buffalo.isMore()},
//         {name: 'startIndex', type: DataType.UINT8, condition: (payload, buffalo) => buffalo && buffalo.isMore()},
//         {name: 'nwks', type: BuffaloZclDataType.LIST_UINT16, options: (payload, options) => (options.length = payload.num)},
//     ],
// },


template<>
struct zb_ncp::cmd_handle<ZDO_IEEE_ADDR_REQ> : request_cmd_process< ZDO_IEEE_ADDR_REQ, zb_zdo_ieee_addr_req_param_t, zb_zdo_ieee_addr_req_param_t, zb_zdo_ieee_addr_resp_t > {
    using Base = request_cmd_process< ZDO_IEEE_ADDR_REQ, zb_zdo_ieee_addr_req_param_t, zb_zdo_ieee_addr_req_param_t, zb_zdo_ieee_addr_resp_t >;
    static constexpr size_t additional_buffer_size = 2+16*2;
    static constexpr bool request_is_data = false;
    static constexpr const char* name = "ZDO_IEEE_ADDR_REQ";
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_IEEE_ADDR_REQ::start_request nwk: %04x nwk_addr: %04x",s_req.dst_addr,s_req.nwk_addr);
        return zb_zdo_ieee_addr_req(buf,&Base::req_cb);
    }
    static void handle_response(ResolveStrategy::request_t& req,const zb_zdo_ieee_addr_resp_t* resp) {
    	uint8_t outdata[Cmd::resp_buffer_size+sizeof(zb_ncp::cmd_t)];
        zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
        *out_cmd = req.cmd;
        out_cmd->type = zb_ncp::RESPONSE;
        auto outlen = sizeof(zb_ncp::cmd_t);
        outlen += Cmd::format_response(reinterpret_cast<uint8_t*>(out_cmd+1),resp);
        if (req.arg.request_type == 0x01) {
        	auto ext = reinterpret_cast<const zb_zdo_ieee_addr_resp_ext_t*>(resp+1);
        	auto dst = reinterpret_cast<uint8_t*>(out_cmd+1)+outlen;
        	auto num = ext->num_assoc_dev;
        	if (num > 16) {
        		ESP_LOGE(TAG,"truncate ZDO_IEEE_ADDR_REQ response %d",int(num));
        		num = 16;
        	}
        	*dst++ = num; ++outlen;
        	if (num) {
        		auto ext2 = reinterpret_cast<const zb_zdo_ieee_addr_resp_ext2_t*>(ext+1);
        		*dst++ = ext2->start_index; ++outlen;
        		auto nwks = reinterpret_cast<const uint8_t*>(ext2+1);
        		// MUST use capped `num`, not ext->num_assoc_dev — the latter is
        		// the untruncated remote-reported count; iterating past `num`
        		// overflows the response stack buffer (additional_buffer_size = 2+16*2).
        		for (uint8_t i=0;i<num;++i) {
        			memcpy(dst, nwks, 2); nwks += 2;  // PROTO-3: ext2+1 and dst can be odd-aligned
        			dst += 2; outlen+=2;
        		}
        	}
        }
        zb_ncp::send_cmd_data( outdata, outlen ); 
    }
   
};

// Request for a remote device NWK address
// [CommandId.ZDO_NWK_ADDR_REQ]: {
//     request: [
//         {name: 'nwk', type: DataType.UINT16},
//         {name: 'ieee', type: DataType.IEEE_ADDR},
//         {name: 'type', type: DataType.UINT8},
//         {name: 'startIndex', type: DataType.UINT8},
//     ],
//     response: [
//         ...commonResponse,
//         {name: 'ieee', type: DataType.IEEE_ADDR},
//         {name: 'nwk', type: DataType.UINT16},
//         {name: 'num', type: DataType.UINT8, condition: (payload, buffalo) => buffalo && buffalo.isMore()},
//         {name: 'startIndex', type: DataType.UINT8, condition: (payload, buffalo) => buffalo && buffalo.isMore()},
//         {name: 'nwks', type: BuffaloZclDataType.LIST_UINT16, options: (payload, options) => (options.length = payload.num)},
//     ],
// },


template<>
struct zb_ncp::cmd_handle<ZDO_NWK_ADDR_REQ> : request_cmd_process< ZDO_NWK_ADDR_REQ, zb_zdo_nwk_addr_req_param_t, zb_zdo_nwk_addr_req_param_t, zb_zdo_nwk_addr_resp_head_t > {
    using Base = request_cmd_process< ZDO_NWK_ADDR_REQ, zb_zdo_nwk_addr_req_param_t, zb_zdo_nwk_addr_req_param_t, zb_zdo_nwk_addr_resp_head_t >;
    static constexpr size_t additional_buffer_size = 2+16*2;
    static constexpr bool request_is_data = false;
    static constexpr const char* name = "ZDO_NWK_ADDR_REQ";
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_IEEE_ADDR_REQ::start_request nwk: %04x nwk_addr: %04x",s_req.dst_addr,s_req.nwk_addr);
        return zb_zdo_nwk_addr_req(buf,&Base::req_cb);
    }
    static void handle_response(ResolveStrategy::request_t& req,const zb_zdo_nwk_addr_resp_head_t* resp) {
    	uint8_t outdata[Cmd::resp_buffer_size+sizeof(zb_ncp::cmd_t)];
        zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
        *out_cmd = req.cmd;
        out_cmd->type = zb_ncp::RESPONSE;
        auto outlen = sizeof(zb_ncp::cmd_t);
        outlen += Cmd::format_response(reinterpret_cast<uint8_t*>(out_cmd+1),resp);
        if (req.arg.request_type == 0x01) {
        	auto ext = reinterpret_cast<const zb_zdo_nwk_addr_resp_ext_t*>(resp+1);
        	auto dst = reinterpret_cast<uint8_t*>(out_cmd+1)+outlen;
        	auto num = ext->num_assoc_dev;
        	if (num > 16) {
        		ESP_LOGE(TAG,"truncate ZDO_NWK_ADDR_REQ response %d",int(num));
        		num = 16;
        	}
        	*dst++ = num; ++outlen;
        	if (num) {
        		auto ext2 = reinterpret_cast<const zb_zdo_nwk_addr_resp_ext2_t*>(ext+1);
        		*dst++ = ext2->start_index; ++outlen;
        		auto nwks = reinterpret_cast<const uint8_t*>(ext2+1);
        		// MUST use capped `num`, see sibling handler ZDO_IEEE_ADDR_REQ.
        		for (uint8_t i=0;i<num;++i) {
        			memcpy(dst, nwks, 2); nwks += 2;  // PROTO-3: ext2+1 and dst can be odd-aligned
        			dst += 2; outlen+=2;
        		}
        	}
        }
        zb_ncp::send_cmd_data( outdata, outlen ); 
    }
   
};

// Get the Power Descriptor from a remote device
// [CommandId.ZDO_POWER_DESC_REQ]: {
//     request: [{name: 'nwk', type: DataType.UINT16}],
//     response: [...commonResponse, {name: 'powerDescriptor', type: DataType.UINT16}, {name: 'nwk', type: DataType.UINT16}],
// },
template<>
struct zb_ncp::cmd_handle<ZDO_POWER_DESC_REQ> : request_cmd_process< ZDO_POWER_DESC_REQ, zb_zdo_power_desc_req_t, zb_zdo_power_desc_req_t, zb_zdo_power_desc_resp_t > {
    using Base = request_cmd_process< ZDO_POWER_DESC_REQ, zb_zdo_power_desc_req_t, zb_zdo_power_desc_req_t, zb_zdo_power_desc_resp_t >;
    static constexpr size_t additional_buffer_size = 0;
    static constexpr bool request_is_data = true;
    static constexpr const char* name = "ZDO_POWER_DESC_REQ";
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_IEEE_ADDR_REQ::start_request nwk: %04x nwk_addr: %04x",s_req.dst_addr,s_req.nwk_addr);
        return zb_zdo_power_desc_req(buf,&Base::req_cb);
    }
     static uint16_t format_response(uint8_t* outdata,const zb_zdo_power_desc_resp_t* resp) {
        outdata[0] = STATUS_CATEGORY_ZDO;
        outdata[1] = resp->hdr.status;
        memcpy(&outdata[2],&resp->power_desc,sizeof(zb_af_node_power_desc_t));
        memcpy(&outdata[2+sizeof(zb_af_node_power_desc_t)], &resp->hdr.nwk_addr, 2);
        return sizeof(zb_zdo_power_desc_resp_t);
    }
   
};


// Send Match Descriptor request to a remote device
// [CommandId.ZDO_MATCH_DESC_REQ]: {
//     request: [
//         {name: 'nwk', type: DataType.UINT16},
//         {name: 'profileID', type: DataType.UINT16},
//         {name: 'inputClusterCount', type: DataType.UINT8},
//         {name: 'outputClusterCount', type: DataType.UINT8},
//         {
//             name: 'inputClusters',
//             type: BuffaloZclDataType.LIST_UINT16,
//             options: (payload, options) => (options.length = payload.inputClusterCount),
//         },
//         {
//             name: 'outputClusters',
//             type: BuffaloZclDataType.LIST_UINT16,
//             options: (payload, options) => (options.length = payload.outputClusterCount),
//         },
//     ],
//     response: [
//         ...commonResponse,
//         {name: 'len', type: DataType.UINT8},
//         {name: 'endpoints', type: BuffaloZclDataType.LIST_UINT8, options: (payload, options) => (options.length = payload.len)},
//         {name: 'nwk', type: DataType.UINT16},
//     ],
// },
// typedef ZB_PACKED_PRE struct zb_zdo_match_desc_param_s
// {
//   zb_uint16_t      nwk_addr;    /*!< NWK address that is used for IEEE
//                                   address mapping.  */
//   zb_uint16_t     addr_of_interest; /*!< NWK address of interest */

//   zb_uint16_t      profile_id;  /*!< Profile ID to be matched at the
//                                   destination.  */
//   zb_uint8_t       num_in_clusters; /*!< The number of Input Clusters
//                                       provided for matching within the
//                                       InClusterList.  */
//   zb_uint8_t       num_out_clusters; /*!< The number of Output Clusters
//                                        provided for matching within
//                                        OutClusterList.  */
//   zb_uint16_t      cluster_list[1]; /*!< Variable size: [num_in_clusters] +  [num_out_clusters]
//                                          List of Input ClusterIDs to be used
//                                          for matching; the InClusterList is
//                                          the desired list to be matched by
//                                          the Remote Device (the elements
//                                          of the InClusterList are the
//                                          supported output clusters of the
//                                          Local Device).
//                                          List of Output ClusterIDs to be
//                                          used for matching; the
//                                          OutClusterList is the desired list to
//                                          be matched by the Remote Device
//                                          (the elements of the
//                                          OutClusterList are the supported
//                                          input clusters of the Local
//                                          Device). */
// }
// ZB_PACKED_STRUCT
// zb_zdo_match_desc_param_t;

// typedef ZB_PACKED_PRE struct zb_zdo_match_desc_resp_s
// {
//   zb_uint8_t tsn;       /*!< ZDP transaction sequence number */
//   zb_uint8_t status;    /*!< The status of the Match_Desc_req command.*/
//   zb_uint16_t nwk_addr; /*!< NWK address for the request. */
//   zb_uint8_t match_len; /*!< The count of endpoints on the Remote Device that match the
//                           request criteria.*/
// }
// ZB_PACKED_STRUCT
// zb_zdo_match_desc_resp_t;

struct ZDO_MATCH_DESC_REQ_arg_t {
	uint16_t nwk;
	uint16_t profileID;
	uint8_t inputClusterCount;
	uint8_t outputClusterCount;
	uint16_t clusters[64];
} __attribute__((packed));

template<>
struct zb_ncp::cmd_handle<ZDO_MATCH_DESC_REQ> : request_cmd_process< ZDO_MATCH_DESC_REQ, ZDO_MATCH_DESC_REQ_arg_t, zb_zdo_match_desc_param_t, zb_zdo_match_desc_resp_t > {
    using Base = request_cmd_process< ZDO_MATCH_DESC_REQ, ZDO_MATCH_DESC_REQ_arg_t, zb_zdo_match_desc_param_t, zb_zdo_match_desc_resp_t >;
    static constexpr size_t additional_buffer_size = 64;
    static constexpr bool request_is_data = true;
    static constexpr const char* name = "ZDO_MATCH_DESC_REQ";
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_IEEE_ADDR_REQ::start_request nwk: %04x nwk_addr: %04x",s_req.dst_addr,s_req.nwk_addr);
        return zb_zdo_match_desc_req(buf,&Base::req_cb);
    }
    static bool check_arg_size(const void *buffer, size_t len) {
    	if (len < 6)
    		return false;
    	auto arg = static_cast<const ZDO_MATCH_DESC_REQ_arg_t*>(buffer);
    	if ((arg->inputClusterCount + arg->outputClusterCount) > 64) {
    		return false;
		}
    	return len >= (6+(arg->inputClusterCount+arg->outputClusterCount)*2);
    }
    static size_t get_request_alloc_size(const ZDO_MATCH_DESC_REQ_arg_t& arg) {
    	return sizeof(zb_zdo_match_desc_param_t)-2+(arg.inputClusterCount+arg.outputClusterCount)*2;
    }
    static void format_request(zb_zdo_match_desc_param_t& req,const ZDO_MATCH_DESC_REQ_arg_t& arg) {
    	req.nwk_addr = arg.nwk;
    	req.addr_of_interest = arg.nwk;
    	req.profile_id = arg.profileID;
    	req.num_in_clusters = arg.inputClusterCount;
    	req.num_out_clusters = arg.outputClusterCount;
    	memcpy(req.cluster_list,arg.clusters,(arg.inputClusterCount+arg.outputClusterCount)*2);
    }
     static uint16_t format_response(uint8_t* outdata,const zb_zdo_match_desc_resp_t* resp) {
     	auto out = outdata;
        *out++ = STATUS_CATEGORY_ZDO;
        *out++ = resp->status;
        auto len = resp->match_len;
        if (len > 64) {
        	ESP_LOGE(TAG,"Truncate ZDO_MATCH_DESC_REQ %d",int(len));
        	len = 64;
        }
        *out++ = len;
        for (uint8_t i=0;i<len;++i) {
        	*out++ = reinterpret_cast<const uint8_t*>(resp+1)[i];
        }
        memcpy(out, &resp->nwk_addr, 2); out += 2;
        return out - outdata;
    }
   
};

// Sends a ZDO Mgmt LQI request to a remote device
// [CommandId.ZDO_MGMT_LQI_REQ]: {
//     request: [
//         {name: 'nwk', type: DataType.UINT16},
//         {name: 'startIndex', type: DataType.UINT8},
//     ],
//     response: [
//         ...commonResponse,
//         {name: 'entries', type: DataType.UINT8},
//         {name: 'startIndex', type: DataType.UINT8},
//         {name: 'len', type: DataType.UINT8},
//         {
//             name: 'neighbors',
//             type: BuffaloZBOSSDataType.LIST_TYPED,
//             typed: [
//                 {name: 'extendedPanID', type: BuffaloZBOSSDataType.EXTENDED_PAN_ID},
//                 {name: 'ieee', type: DataType.IEEE_ADDR},
//                 {name: 'nwk', type: DataType.UINT16},
//                 {name: 'relationship', type: DataType.UINT8},
//                 {name: 'joining', type: DataType.UINT8},
//                 {name: 'depth', type: DataType.UINT8},
//                 {name: 'lqi', type: DataType.UINT8},
//             ],
//             options: (payload, options) => (options.length = payload.len),
//         },
//     ],
// },

struct S_ZDO_MGMT_LQI_REQ_arg_t {
    uint16_t nwk;
    uint8_t startIndex;
}__attribute__((packed));

template<>
struct zb_ncp::cmd_handle<ZDO_MGMT_LQI_REQ> : request_cmd_process< ZDO_MGMT_LQI_REQ, S_ZDO_MGMT_LQI_REQ_arg_t, zb_zdo_mgmt_lqi_param_t, zb_zdo_mgmt_lqi_resp_t > {
    using Base = request_cmd_process< ZDO_MGMT_LQI_REQ, S_ZDO_MGMT_LQI_REQ_arg_t, zb_zdo_mgmt_lqi_param_t, zb_zdo_mgmt_lqi_resp_t >;
    static constexpr bool request_is_data = false;
    static constexpr size_t additional_buffer_size = 64*sizeof(zb_zdo_neighbor_table_record_t);
    static constexpr const char* name = "ZDO_MGMT_LQI_REQ";
    static void format_request(zb_zdo_mgmt_lqi_param_t& req,const S_ZDO_MGMT_LQI_REQ_arg_t& arg) {
    	req.dst_addr = arg.nwk;
        req.start_index = arg.startIndex;
    }
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_MGMT_LQI_REQ::start_request dst_addr: %04x start_index: %d",s_req.dst_addr,int(s_req.start_index));
        return zb_zdo_mgmt_lqi_req(buf,&Base::req_cb);
    }
    static uint16_t format_response(uint8_t* outdata,const zb_zdo_mgmt_lqi_resp_t* resp) {
        auto len = Base::format_response(outdata,resp);
        auto outwrite = &outdata[len];
        auto src = reinterpret_cast<const zb_zdo_neighbor_table_record_t*>(resp+1);
        auto cnt = resp->neighbor_table_list_count;
        if (cnt > 64) {
        	ESP_LOGE(TAG,"Truncate ZDO_MGMT_LQI_REQ %d",int(resp->neighbor_table_entries));
        	cnt = 64;
        	reinterpret_cast<zb_zdo_mgmt_lqi_resp_t*>(outdata)->neighbor_table_list_count = cnt;
        }
        ESP_LOGD(TAG,"ZDO_MGMT_LQI_REQ format_response entries:%d start_index: %d len:%d",int(resp->start_index),int(cnt),int(cnt));
        for (uint8_t i=0;i<cnt;++i) {
            memcpy(outwrite,src,sizeof(zb_zdo_neighbor_table_record_t));
            outwrite += sizeof(zb_zdo_neighbor_table_record_t);
            src++;
        }
        return outwrite-outdata;
    }
};

// esp-zigbee-lib device-leave API (consumed by the ZDO_MGMT_LEAVE_REQ handler).
#include "zdo/esp_zigbee_zdo_command.h"

// Request that a Remote Device leave the network
// [CommandId.ZDO_MGMT_LEAVE_REQ]: {
//     request: [
//         {name: 'nwk', type: DataType.UINT16},
//         {name: 'ieee', type: DataType.IEEE_ADDR},
//         {name: 'flags', type: DataType.UINT8},
//     ],
//     response: [...commonResponse],
// },

struct ZDO_MGMT_LEAVE_REQ_args_t {
    uint16_t short_addr;
    uint8_t long_addr[8];
    uint8_t flags;
}__attribute__((packed));
// LEAVE-GATE + safe removal (2026-06-19, bench-characterized root cause of the
// field "spontaneous wipe", Discussion #1). The LOW-LEVEL zdo_mgmt_leave_req()
// this handler used to call makes the coordinator intermittently (~60-75% per
// leave, bench-measured) perform an NLME-LEAVE on SELF -> it silently wipes its
// OWN network (live PIB + persisted NVRAM), live, no reboot. The wipe is target-
// and param-independent (unknown short, self 0x0000, and a freshly joined child
// all wipe; DevAddr=0 wipes as much as DevAddr=ieee). z2m sends this leave on
// every device force-remove -> the field "spontaneous wipe".
//
// FIX: route through the OFFICIAL esp-zigbee-lib API esp_zb_zdo_device_leave_req()
// instead of the raw low-level call. Bench A/B (temp diag cmd, since removed):
// the low-level call wiped ~2/3 for EVERY target; the esp_zb wrapper wiped 0/12
// for a remote target and a real joined child survived -> the bug is specific to
// the raw low-level call. The ONE residual case the wrapper still self-wipes is a
// leave addressed to dst=0x0000 (the coordinator itself: 6/6), which is
// semantically "I, the coordinator, leave my own network" -- z2m never sends that
// for a device removal, so we refuse it. Immediate handler (mirrors
// NWK_PERMIT_JOINING): reply synchronously, fire the leave async. Runs under the
// dispatch's zboss_lock_guard, so esp_zb_* is safe here (same recursive osif lock
// that esp_zb_lock_acquire takes).
template<>
struct zb_ncp::cmd_handle<ZDO_MGMT_LEAVE_REQ> : immediate_cmd_process<ZDO_MGMT_LEAVE_REQ> {
    static constexpr size_t resp_buffer_size = sizeof(generic_response_t);
    static size_t process_immediate(const void* inbuf, size_t inlen, uint8_t* outdata, size_t outdata_size) {
        auto resp = reinterpret_cast<generic_response_t*>(outdata);
        resp->category = STATUS_CATEGORY_ZDO;   // byte-compatible with the old request_cmd_process reply
        if (inlen < sizeof(ZDO_MGMT_LEAVE_REQ_args_t)) {
            resp->status = GENERIC_INVALID_PARAMETER;
            return sizeof(generic_response_t);
        }
        ZDO_MGMT_LEAVE_REQ_args_t a;
        memcpy(&a, inbuf, sizeof(a));   // unaligned-safe copy out of the wire buffer
        if (a.short_addr == 0x0000) {
            ESP_LOGW(TAG, "ZDO_MGMT_LEAVE_REQ: refusing self-addressed leave (dst=0x0000 would wipe the coordinator)");
            resp->status = GENERIC_OPERATION_FAILED;
            return sizeof(generic_response_t);
        }
        esp_zb_zdo_mgmt_leave_req_param_t p; memset(&p, 0, sizeof(p));
        memcpy(p.device_address, a.long_addr, 8);
        p.dst_nwk_addr = a.short_addr;
        p.rejoin = (a.flags & 0x80) ? 1 : 0;
        esp_zb_zdo_device_leave_req(&p, [](esp_zb_zdp_status_t, void*){}, nullptr);
        resp->status = GENERIC_OK;   // leave initiated; the target leaves asynchronously
        return sizeof(generic_response_t);
    }
};

// [CommandId.ZDO_MGMT_BIND_REQ]: {
    //     request: [
    //         {name: 'nwk', type: DataType.UINT16},
    //         {name: 'startIndex', type: DataType.UINT8},
    //     ],
    //     response: [...commonResponse],
    // },
struct ZDO_MGMT_BIND_REQ_args_t {
    uint16_t nwk;
    uint8_t startIndex;
}__attribute__((packed));
template<>
struct zb_ncp::cmd_handle<ZDO_MGMT_BIND_REQ> : request_cmd_process< ZDO_MGMT_BIND_REQ, ZDO_MGMT_BIND_REQ_args_t, zb_zdo_mgmt_bind_param_t, zb_zdo_mgmt_bind_resp_t > {
    using Base = request_cmd_process< ZDO_MGMT_BIND_REQ, ZDO_MGMT_BIND_REQ_args_t, zb_zdo_mgmt_bind_param_t, zb_zdo_mgmt_bind_resp_t >;
    static constexpr size_t additional_buffer_size = 64 * sizeof(zb_zdo_binding_table_record_t);
    static constexpr const char* name = "ZDO_MGMT_BIND_REQ";
    static void format_request(zb_zdo_mgmt_bind_param_t& req, const ZDO_MGMT_BIND_REQ_args_t& arg) {
        memset(&req,0,sizeof(zb_zdo_mgmt_bind_param_t));
        req.dst_addr = arg.nwk;
        req.start_index = arg.startIndex;
    }
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_MGMT_LEAVE_REQ start_request device_address: " IEEE_ADDR_FMT " dst_addr:%04x ",IEEE_ADDR_PRINT(s_req.device_address),s_req.dst_addr);
        return zb_zdo_mgmt_bind_req(buf,&Base::req_cb);
    }
    // const sourceEui64 = this.readIeeeAddr();
    // const sourceEndpoint = this.readUInt8();
    // const clusterId = this.readUInt16();
    // const destAddrMode = this.readUInt8();
    // const dest = destAddrMode === 0x01 ? this.readUInt16() : destAddrMode === 0x03 ? this.readIeeeAddr() : undefined;
    // const destEndpoint = destAddrMode === 0x03 ? this.readUInt8() : undefined;

    // if (dest === undefined) {
    //     // not supported (using reserved value)
    //     continue;
    // }

    static uint16_t format_response(uint8_t* outdata,const zb_zdo_mgmt_bind_resp_t* resp) {
        auto len = Base::format_response(outdata,resp);
        auto out = &outdata[len];
        auto src = reinterpret_cast<const zb_zdo_binding_table_record_t*>(resp+1);
        auto cnt = resp->binding_table_list_count;
        if (cnt > 64) {
        	ESP_LOGE(TAG,"Truncate ZDO_MGMT_BIND_REQ %d",int(cnt));
        	cnt = 64;
        	reinterpret_cast<zb_zdo_mgmt_bind_resp_t*>(outdata)->binding_table_list_count = cnt;
        }
        // H8: write each record field-by-field so the wire layout no longer
        // depends on zb_zdo_binding_table_record_t happening to match the
        // ZDP wire format byte-for-byte (incl. dst_address union packing).
        // Pre-fix code memcpy'd sizeof(record) then advanced `outwrite` by
        // only the per-mode wire size, relying on the next iteration's memcpy
        // to overwrite the leftover tail. Worked by accident for mode 1/3 and
        // emitted garbage tail bytes after the last record for any other
        // dst_addr_mode value.
        for (uint8_t i=0;i<cnt;++i) {
            memcpy(out, src->src_address, 8); out += 8;
            *out++ = src->src_endp;
            memcpy(out, &src->cluster_id, 2); out += 2;
            *out++ = src->dst_addr_mode;
            if (src->dst_addr_mode == 0x01) {
                memcpy(out, &src->dst_address.addr_short, 2); out += 2;
            } else if (src->dst_addr_mode == 0x03) {
                memcpy(out, src->dst_address.addr_long, 8); out += 8;
                *out++ = src->dst_endp;
            } else {
                // Reserved/unknown mode — record ends at dst_addr_mode.
                // zigbee-herdsman parses the rest of the buffer record-by-
                // record and treats undefined dst as 'skip', so emitting no
                // extra payload matches its expectations.
                ESP_LOGW(TAG,"ZDO_MGMT_BIND_REQ: unknown dst_addr_mode %d",
                         int(src->dst_addr_mode));
            }
            ++src;
        }
        return out-outdata;
    }
};

// Sends a ZDO Mgmt NWK Update Request to a remote device
// [CommandId.ZDO_MGMT_NWK_UPDATE_REQ]: {
//     request: [
//         {name: 'channelMask', type: DataType.UINT32},
//         {name: 'duration', type: DataType.UINT8},
//         {name: 'count', type: DataType.UINT8},
//         {name: 'managerNwk', type: DataType.UINT16},
//         {name: 'nwk', type: DataType.UINT16},
//     ],
//     response: [...commonResponse],


template<>
struct zb_ncp::cmd_handle<ZDO_MGMT_NWK_UPDATE_REQ> : request_cmd_process< ZDO_MGMT_NWK_UPDATE_REQ, zb_zdo_mgmt_nwk_update_req_t, zb_zdo_mgmt_nwk_update_req_s, zb_zdo_mgmt_nwk_update_notify_hdr_t > {
    using Base = request_cmd_process< ZDO_MGMT_NWK_UPDATE_REQ, zb_zdo_mgmt_nwk_update_req_t, zb_zdo_mgmt_nwk_update_req_s, zb_zdo_mgmt_nwk_update_notify_hdr_t >;
    static constexpr size_t additional_buffer_size = 64;
    static constexpr bool request_is_data = false;
    static constexpr const char* name = "ZDO_MGMT_NWK_UPDATE_REQ";
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_MGMT_LEAVE_REQ start_request device_address: " IEEE_ADDR_FMT " dst_addr:%04x ",IEEE_ADDR_PRINT(s_req.device_address),s_req.dst_addr);
        return zb_zdo_mgmt_nwk_update_req(buf,&Base::req_cb);
    }
    static uint16_t format_response(uint8_t* outdata,const zb_zdo_mgmt_nwk_update_notify_hdr_t* resp) {
        auto len = Base::format_response(outdata,resp);
        auto outwrite = &outdata[len];
        auto src = reinterpret_cast<const uint8_t*>(resp+1);
        auto cnt = resp->scanned_channels_list_count;
        if (cnt > 64) {
        	ESP_LOGE(TAG,"Truncate ZDO_MGMT_NWK_UPDATE_REQ %d",int(cnt));
        	cnt = 64;
        	reinterpret_cast<zb_zdo_mgmt_nwk_update_notify_hdr_t*>(outdata)->scanned_channels_list_count = cnt;
        }
        for (uint8_t i=0;i<cnt;++i) {
        	*outwrite++ = *src++;
        }
        return outwrite-outdata;
    }
};

// APSDE-DATA.request
// [CommandId.APSDE_DATA_REQ]: {
//     request: [
//         {name: 'paramLength', type: DataType.UINT8},
//         {name: 'dataLength', type: DataType.UINT16},
//         {name: 'addr', type: DataType.IEEE_ADDR},
//         {name: 'profileID', type: DataType.UINT16},
//         {name: 'clusterID', type: DataType.UINT16},
//         {name: 'dstEndpoint', type: DataType.UINT8, condition: (payload) => [2, 3].includes(payload.dstAddrMode)},
//         //{name: 'dstEndpoint', type: DataType.UINT8},
//         {name: 'srcEndpoint', type: DataType.UINT8},
//         {name: 'radius', type: DataType.UINT8},
//         {name: 'dstAddrMode', type: DataType.UINT8},
//         {name: 'txOptions', type: DataType.UINT8},
//         {name: 'useAlias', type: DataType.UINT8},
//         //{name: 'aliasAddr', type: DataType.UINT16, condition: (payload) => payload.useAlias !== 0},
//         {name: 'aliasAddr', type: DataType.UINT16},
//         {name: 'aliasSequence', type: DataType.UINT8},
//         {name: 'data', type: BuffaloZclDataType.LIST_UINT8, options: (payload, options) => (options.length = payload.dataLength)},
//     ],
//     response: [
//         ...commonResponse,
//         {name: 'ieee', type: DataType.IEEE_ADDR},
//         {name: 'dstEndpoint', type: DataType.UINT8, condition: (payload) => [2, 3].includes(payload.dstAddrMode)},
//         {name: 'srcEndpoint', type: DataType.UINT8},
//         {name: 'txTime', type: DataType.UINT32},
//         {name: 'dstAddrMode', type: DataType.UINT8},
//     ],
// },
// const payload = {
//             paramLength: 20,
//             dataLength: data.length,
//             addr: `0x${group.toString(16).padStart(16, '0')}`,
//             profileID: profileID,
//             clusterID: clusterID,
//             srcEndpoint: srcEp,
//             radius: 3,
//             dstAddrMode: 1, // ADDRESS MODE group
//             txOptions: 2, // ROUTE DISCOVERY
//             useAlias: 0,
//             aliasAddr: 0,
//             aliasSequence: 0,
//             data: data,
//         };

struct apsde_data_req_base_t {
        uint8_t addr_data[8];
        uint16_t profile_id; // 0x08
        uint16_t cluster_id; // 0x0a
        uint8_t dst_endpoint; // 0x0c
        uint8_t src_endpoint; // 0x0d
        uint8_t radius;     // 0x0e
        uint8_t addr_mode;  // 0x0f;
        uint8_t tx_options; // 0x10
        uint8_t use_alias;  // 0x11
        uint16_t alias_src_addr; // 0x12
        uint8_t alias_seq_num; // 0x14
} __attribute__((packed));

struct apsde_data_req_base_nep_t {
        uint8_t addr_data[8];
        uint16_t profile_id; // 0x08
        uint16_t cluster_id; // 0x0a
        uint8_t src_endpoint; // 0x0d
        uint8_t radius;     // 0x0e
        uint8_t addr_mode;  // 0x0f;
        uint8_t tx_options; // 0x10
        uint8_t use_alias;  // 0x11
        uint16_t alias_src_addr; // 0x12
        uint8_t alias_seq_num; // 0x14
} __attribute__((packed));

struct apsde_data_req_t {
    apsde_data_req_base_t base ;
    uint8_t _unknown2[0x1a-0x15];
} __attribute__((packed));
static_assert(sizeof(apsde_data_req_t)==0x1a);

struct apsde_data_req_arg_t {
	uint8_t paramLength;
    uint16_t dataLength;
    apsde_data_req_base_t base;
} __attribute__((packed));

struct apsde_data_req_arg_nep_t {
	uint8_t paramLength;
    uint16_t dataLength;
    apsde_data_req_base_nep_t base;
} __attribute__((packed));

static constexpr size_t MAX_APSDE_DATA_REQ_SIZE = 256;

struct APSDE_DATA_REQ_max_arg_t {
	apsde_data_req_arg_t hdr;
	uint8_t data[MAX_APSDE_DATA_REQ_SIZE];
} __attribute__((packed));

struct APSDE_DATA_REQ_max_arg_nep_t {
	apsde_data_req_arg_nep_t hdr;
	uint8_t data[MAX_APSDE_DATA_REQ_SIZE];
} __attribute__((packed));


struct zb_apsde_data_resp_t {
    uint8_t addr[8];        // 0
    uint8_t dst_endpoint;   // 8
    uint8_t src_endpoint;   // 9
    uint32_t tx_time;       // 10
    uint8_t _unknown1[0x12-(10+4)];     
    uint8_t dst_addr_mode;  // 0x12
    uint8_t status; // 0x13
    uint8_t _unknown2[0x19-0x14];
} __attribute__((packed));

static_assert(sizeof(zb_apsde_data_resp_t) == 0x19);

template<>
struct zb_ncp::cmd_handle<APSDE_DATA_REQ> : request_cmd_resolver<APSDE_DATA_REQ,APSDE_DATA_REQ_max_arg_t>, cmd_base<cmd_handle<APSDE_DATA_REQ>> {
    static constexpr const char* name = "APSDE_DATA_REQ";
    using Cmd = cmd_handle<APSDE_DATA_REQ>;
    using CmdBase = cmd_base< zb_ncp::cmd_handle<APSDE_DATA_REQ> >;
    using ResolveStrategy = request_cmd_resolver<APSDE_DATA_REQ,APSDE_DATA_REQ_max_arg_t>;
    using Arg = apsde_data_req_arg_t;

 //         {name: 'ieee', type: DataType.IEEE_ADDR},
//         {name: 'dstEndpoint', type: DataType.UINT8, condition: (payload) => [2, 3].includes(payload.dstAddrMode)},
//         {name: 'srcEndpoint', type: DataType.UINT8},
//         {name: 'txTime', type: DataType.UINT32},
//         {name: 'dstAddrMode', type: DataType.UINT8},

    static void handle_response(ResolveStrategy::request_t& req,const zb_apsde_data_resp_t* resp) {
    	// C1: worst case = mode-2/3 unicast confirm: cmd_t | category(1) |
    	// status(1) | ieee(8) | dst_endpoint(1, only for dst_addr_mode 2/3) |
    	// src_endpoint(1) | tx_time(4) | dst_addr_mode(1). The old size omitted
    	// category + dst_endpoint -> 1-2 B OOB stack write + leak to the host on
    	// every APSDE confirm.
    	uint8_t outdata[sizeof(zb_ncp::cmd_t)+1+1+8+1+1+4+1];
    	zb_ncp::cmd_t* out_cmd = reinterpret_cast<zb_ncp::cmd_t*>(outdata);
        *out_cmd = req.cmd;
        out_cmd->type = zb_ncp::RESPONSE;
        auto out = reinterpret_cast<uint8_t*>(out_cmd+1);
        *out++ = STATUS_CATEGORY_APS;
        *out++ = 0;
        memcpy(out,resp->addr,8); out+=8;
        if (resp->dst_addr_mode == 2 || resp->dst_addr_mode == 3) {
        	*out++ = resp->dst_endpoint;
        }
        *out++ = resp->src_endpoint;
        memcpy(out, &resp->tx_time, 4); out += 4;
        *out++ = resp->dst_addr_mode;
        zb_ncp::send_cmd_data( outdata, out-outdata ); 
    }
    static void aps_user_payload_callback(uint8_t param) {
        if (param) {
            zb_apsde_data_resp_t *ind = ZB_BUF_GET_PARAM(param, zb_apsde_data_resp_t);
            uint8_t len = 0;
            auto data_ptr = static_cast<const uint8_t*>(zb_aps_get_aps_payload(param,&len));

            ESP_LOGD(TAG,"aps_user_payload_callback %d from %d hdr: %d addr:" IEEE_ADDR_FMT " tsn: %d len: %d:%p",
            	ind->dst_endpoint,ind->src_endpoint,
            	IEEE_ADDR_PRINT(ind->addr),int(ind->dst_addr_mode),int(data_ptr[1]),int(len),data_ptr);

            // DISP-2: claim+free the slot atomically before the blocking
            // handle_response/report_failed send. data_ptr[1] is the ZCL TSN.
            ResolveStrategy::request_t req;
            if (ResolveStrategy::resolve_take(data_ptr[1], req)) {
            	ESP_LOGD(TAG,"%s::aps_user_payload_callback %d",Cmd::name,int(data_ptr[1]));
	        	if (ind->status == 0) {
	        		 Cmd::handle_response(req,ind);
	        	} else {
	        		report_failed(req.cmd,ind->status);
	        	}
            } else {
            	ESP_LOGW(TAG,"%s not found request for response %d",Cmd::name,int(data_ptr[1]));
            }


            zb_buf_free(param);
        }

    }

    static uint8_t get_dst_endpoint(const apsde_data_req_base_t& d) {
    	return d.dst_endpoint;
    }

    static uint8_t get_dst_endpoint(const apsde_data_req_base_nep_t& d) {
    	return 0;
    }

    template <typename ArgVar>
    static zb_ret_t start_request(uint8_t buf,ArgVar& arg,ResolveStrategy::request_t& req) {
    	zb_addr_u dst_addr;
        memcpy(&dst_addr,arg.hdr.base.addr_data,8);

    	if (arg.hdr.base.tx_options & 2) {

        } else {

        }
        req.tsn = arg.data[1];
        ESP_LOGD(TAG,"zb_aps_send_user_payload %d -> %d, %d len: %d",int(arg.hdr.base.src_endpoint),
        	int(get_dst_endpoint(arg.hdr.base)),int(req.tsn),int(arg.hdr.dataLength));
        auto ret = zb_aps_send_user_payload(buf,
            dst_addr,
            arg.hdr.base.profile_id,
            arg.hdr.base.cluster_id,
            get_dst_endpoint(arg.hdr.base),
            arg.hdr.base.src_endpoint,
            arg.hdr.base.addr_mode,
            ZB_TRUE, //s->base.tx_options & 1 ? ZB_FALSE : ZB_TRUE,
            arg.data,
            arg.hdr.dataLength);

        return ret;
    }
    static void do_request(uint8_t buf,uint16_t req_arg) {

    	zb_aps_set_user_data_tx_cb(&aps_user_payload_callback);

    	// Req* request_data;
    	auto& req = ResolveStrategy::get_by_index(req_arg);

    	// DISP-2: the slot is S_ALLOCATION (reserved by start_resolve), so reading
    	// req.arg and letting start_request set req.tsn here is safe — no other
    	// task matches an S_ALLOCATION slot. Publish S_EXEC only AFTER the send so
    	// a concurrent app-task start_resolve override can't re-pick this slot
    	// mid-send and tear req.arg out from under start_request. The aps tx
    	// confirm (aps_user_payload_callback) is a deferred ZBOSS-task callback —
    	// it runs after do_request returns, never synchronously inside
    	// start_request — so publishing S_EXEC after the send still races nothing.
        zb_ret_t ret;
    	if (req.arg.hdr.paramLength == 21) {
			auto& arg = req.arg;
			ret = start_request(buf,arg,req);
    	} else {
    		auto& arg = *reinterpret_cast<APSDE_DATA_REQ_max_arg_nep_t*>(&req.arg);
    		ret = start_request(buf,arg,req);
    	}
    	if (ret != 0) {
    		ESP_LOGE(TAG,"failed zb_aps_send_user_payload %02x",int(ret));
    		zb_ncp::cmd_t failed_cmd;
    		{	utils::sem_lock l(request_resolver_mutex());
    			failed_cmd = req.cmd;
    			req.state = ResolveStrategy::request_t::S_NONE;
    		}
    		report_failed(failed_cmd,ret);
    		return;
    	}
    	utils::sem_lock l(request_resolver_mutex());
    	req.t_exec = xTaskGetTickCount();
    	req.state = ResolveStrategy::request_t::S_EXEC;
    }
    static void process(const zb_ncp::cmd_t& cmd, const void *buffer, size_t len) {
    	// Smallest acceptable frame: nep header (no dst_endpoint), zero data bytes.
    	if (len < sizeof(apsde_data_req_arg_nep_t)) {
    		ESP_LOGE(TAG,"APSDE_DATA_REQ: short frame %zu < %zu",
    		         len, sizeof(apsde_data_req_arg_nep_t));
    		report_failed(cmd,GENERIC_INVALID_PARAMETER);
    		return;
    	}
    	if (len > sizeof(APSDE_DATA_REQ_max_arg_t)) {
    		ESP_LOGE(TAG,"APSDE_DATA_REQ: oversize frame %zu > %zu",
    		         len, sizeof(APSDE_DATA_REQ_max_arg_t));
    		report_failed(cmd,GENERIC_INVALID_PARAMETER);
    		return;
    	}
    	auto arg = static_cast<const Arg*>(buffer);
    	if (arg->paramLength != 20 && arg->paramLength != 21) {
    		report_failed(cmd,GENERIC_INVALID_PARAMETER);
    		return;
    	}

    	// M3: dataLength is host-supplied — must match the bytes actually
    	// present in the frame after the param header. Without this check a
    	// frame with paramLength=20, dataLength=0xFFFF, len=24 reached
    	// zb_aps_send_user_payload(..., arg.data, 0xFFFF) and read 65 KiB
    	// past the end of the 256-byte data[] array.
    	const size_t hdr_size = (arg->paramLength == 21)
    		? sizeof(apsde_data_req_arg_t)
    		: sizeof(apsde_data_req_arg_nep_t);
    	if (len < hdr_size) {
    		ESP_LOGE(TAG,"APSDE_DATA_REQ: len %zu < hdr_size %zu (paramLength=%u)",
    		         len, hdr_size, unsigned(arg->paramLength));
    		report_failed(cmd,GENERIC_INVALID_PARAMETER);
    		return;
    	}
    	const size_t actual_data_len = len - hdr_size;
    	if (arg->dataLength != actual_data_len || arg->dataLength > MAX_APSDE_DATA_REQ_SIZE) {
    		ESP_LOGE(TAG,"APSDE_DATA_REQ: dataLength %u != actual %zu (max %zu)",
    		         unsigned(arg->dataLength), actual_data_len, size_t(MAX_APSDE_DATA_REQ_SIZE));
    		report_failed(cmd,GENERIC_INVALID_PARAMETER);
    		return;
    	}
    	// M4: start_request and aps_user_payload_callback both read arg.data[1]
    	// as the ZCL frame TSN for response matching. Need >= 2 data bytes.
    	if (arg->dataLength < 2) {
    		ESP_LOGE(TAG,"APSDE_DATA_REQ: dataLength %u too small for ZCL header",
    		         unsigned(arg->dataLength));
    		report_failed(cmd,GENERIC_INVALID_PARAMETER);
    		return;
    	}

    	auto req = ResolveStrategy::start_resolve(cmd);
    	if (!req) {
    		report_failed(cmd,GENERIC_OUT_OF_RANGE);
    		return;
    	}
    	req->state = ResolveStrategy::request_t::S_ALLOCATION;
    	memcpy(&req->arg,buffer,len);
    	auto ret = zb_buf_get_out_delayed_ext(&do_request,ResolveStrategy::get_req_idx(req),len);

    	// 
        // auto ret = zb_buf_get_out_delayed_ext(&do_request,ResolveStrategy::get_req_idx(req),sizeof(Req));
        if (ret != 0) {
        	req->state = ResolveStrategy::request_t::S_NONE;
        	report_failed(cmd,GENERIC_NO_RESOURCES);
        } else {
        	ESP_LOGD(TAG,"%s::do_start",Cmd::name);
        }
        // return ESP_OK;
    }
};


static void dummy_cb_manuf_code(zb_ret_t status) {}

template <>
struct zb_ncp::cmd_handle<ZDO_SET_NODE_DESC_MANUF_CODE> : immediate_cmd_process<ZDO_SET_NODE_DESC_MANUF_CODE>,
		general_status_arg<ZDO_SET_NODE_DESC_MANUF_CODE,uint16_t> {
	static void process_status_arg(ncp_generic_status_t& status, uint16_t arg) {
		ESP_LOGI(TAG, "ZDO_SET_NODE_DESC_MANUF_CODE: 0x%04X", arg);
        zb_set_node_descriptor_manufacturer_code_req(arg, dummy_cb_manuf_code);
    }
};

#include "esp_partition.h"

// Cached partition handles for the network-backup commands. Looked up once and
// reused for every chunk — esp_partition_find_first is not free.
static inline const esp_partition_t* backup_nvs_partition() {
    static const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    return p;
}
static inline const esp_partition_t* backup_zb_partition() {
    static const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "zb_storage");
    return p;
}

// Session state for chunked RESTORE_NETWORK. The handler runs on the single app
// event-loop task, so no synchronisation is needed. `active` becomes true on the
// offset==0 chunk and false again when the final chunk completes or a fresh
// session starts; `next_offset` enforces monotonic chunk arrival (C2 in audit).
struct restore_session_t {
    uint32_t next_offset;
    uint32_t total_size;
    bool     active;
};
static restore_session_t s_restore_session = { 0, 0, false };

// GET_NETWORK_BACKUP RAM snapshot (pre-release hardening 2026-06-05). The
// chunked pull used to read flash 320 times spread over the whole transfer
// (~7-30 s); when that overlapped ZBOSS NVRAM (re)write activity shortly
// after a boot (herdsman resets the NCP at every adapter start; heaviest
// after a RESTORE_NETWORK reboot or a post-erase formation), single chunk
// responses stalled >10 s and the host aborted — the long-standing v1.1.22
// "backup timeout" follow-up, reproduced near-deterministically 2026-06-05
// (offsets 4992-5760) during the UART-transport validation. Serving all
// chunks from a snapshot taken at offset==0 shrinks the flash-access window
// to one bulk read AND makes the image a consistent point-in-time copy
// instead of a live-mutating read. Single static slot; freed when the last
// chunk is served, reused/re-read on every offset==0. Runs on the app event
// task only — no locking needed.
static uint8_t* s_backup_snapshot = nullptr;
static uint32_t s_backup_snapshot_size = 0;

// Feature 4 (buffer-before-commit restore gate): the FULL 40 KB restore image
// (nvs + zb_storage halves) is accumulated in RAM here, and NOTHING is written
// to flash until the final chunk passes the gross-garbage gate. So a rejected
// (gross-garbage) restore is side-effect-free -- the user's existing network
// and WiFi creds survive untouched, with no reboot. nullptr => not buffering
// (no active restore, or the malloc declined at session start => legacy
// stream-to-flash: erase-upfront + always-reboot, no gate). Freed at session
// end / abandon / restart. Single static slot, app event task only -- no lock.
// Review #4 (LOW): if a host opens a buffered session and then walks away
// WITHOUT finishing and without an NCP_RESET, this 40 KB block stays allocated.
// Bounded + self-healing: the next RESTORE (offset==0) frees it via
// restore_free_buf(), and any reboot (NCP_RESET / commit / a dirtied-flash
// abort) clears the static outright. This USB-only master has no host-disconnect
// link-reset hook (coex's release_backup_snapshot reclaim does not exist here),
// so there is no cheaper deterministic free point; the bound is one block.
static uint8_t* s_restore_img = nullptr;
static void restore_free_buf() {
    if (s_restore_img) { free(s_restore_img); s_restore_img = nullptr; }
}

// Review #1 (HIGH): once flash has been physically touched this boot -- the
// legacy malloc-declined path erases nvs+zb_storage upfront, and the buffered
// commit path erases before writing -- the coordinator can NO LONGER be left
// running on a half-/de-init'd NVS. So from that point on, EVERY terminal exit
// (incl. a later buffered gate-reject from a *different* session, or a
// mid-stream protocol error) MUST reboot, where app::init re-mounts a clean
// partition next boot. This is a per-boot latch: set once, never cleared until
// the reboot it forces. Without it, the sequence "legacy session erases ->
// host abandons -> a fresh buffered session rejects and returns 'preserved'"
// leaves the device on dead NVS until a power-cycle.
static bool s_restore_flash_dirty   = false;
static bool s_restore_reboot_pending = false;

static void restore_schedule_reboot() {
    if (s_restore_reboot_pending) return;   // idempotent: one reboot task only
    s_restore_reboot_pending = true;
    ESP_LOGI(TAG, "RESTORE_NETWORK: rebooting in 1s");
    xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(1000)); esp_restart(); },
                "reboot", 2048, NULL, 5, NULL);
}

static size_t restore_reply(uint8_t* outdata, ncp_generic_status_t status) {
    auto r = reinterpret_cast<generic_response_t*>(outdata);
    r->category = STATUS_CATEGORY_GENERIC;
    r->status   = status;
    return sizeof(generic_response_t);
}

// Terminal failure exit for RESTORE_NETWORK. Frees the RAM buffer and closes
// the session (review #3: mid-session validation failures used to leak the
// 40 KB s_restore_img and leave `active` set), and -- if flash was already
// dirtied this boot -- schedules the reboot (review #1). A pure buffered
// reject that never touched flash stays side-effect-free (no reboot): that is
// the buffer-before-commit guarantee.
static size_t restore_fail(uint8_t* outdata, ncp_generic_status_t status) {
    restore_free_buf();
    s_restore_session.active = false;
    if (s_restore_flash_dirty) restore_schedule_reboot();
    return restore_reply(outdata, status);
}

template <>
struct zb_ncp::cmd_handle<GET_NETWORK_BACKUP> : immediate_cmd_process<GET_NETWORK_BACKUP> {
    static constexpr size_t resp_buffer_size = sizeof(generic_response_t) + 8 + 128;
    static size_t process_immediate(const void *inbuffer, size_t inlen, uint8_t* outdata, size_t outdata_size) {
        // Unaligned-safe read of offset (RV32 fault risk if cast as uint32_t*).
        uint32_t offset = 0;
        if (inlen >= 4) {
            memcpy(&offset, inbuffer, 4);
        }

        auto full_res = reinterpret_cast<generic_response_t*>(outdata);
        full_res->category = STATUS_CATEGORY_GENERIC;
        full_res->status = GENERIC_OK;
        uint8_t* payload = outdata + sizeof(generic_response_t);

        const auto nvs_part = backup_nvs_partition();
        const auto zb_part  = backup_zb_partition();
        if (!nvs_part || !zb_part) {
            ESP_LOGE(TAG, "GET_NETWORK_BACKUP: partitions missing (nvs=%p zb=%p)", nvs_part, zb_part);
            full_res->status = GENERIC_OPERATION_FAILED;
            const uint32_t zero = 0;
            memcpy(payload,     &zero, 4);
            memcpy(payload + 4, &zero, 4);
            return sizeof(generic_response_t) + 8;
        }

        // Total size is derived from the actual partition layout, not a
        // hardcoded constant — re-flashing with a different partition table
        // no longer silently truncates / over-reads (C3).
        const uint32_t total_size = nvs_part->size + zb_part->size;

        // (Re)build the RAM snapshot on every session start (offset==0) so a
        // restarted pull gets fresh data. Allocation/read failure degrades to
        // the original per-chunk direct reads below.
        if (offset == 0) {
            // === BACKUP-1 poison-backup guard (root-caused 2026-06-19, Disc#1/
            // peca89; bench-proven on a real C6). A coordinator whose ZBOSS
            // NVRAM corrupted during operation (e.g. an interrupted flash write
            // from a power/USB glitch) boots factory-blank (joined=0, panID
            // 0xFFFF) while its flash still physically holds the old, now-
            // UNLOADABLE network bytes. GET_NETWORK_BACKUP used to read those
            // raw bytes and hand them to the host as a "valid" image (the PAN is
            // visibly present, so no naive check catches it); RESTORE then
            // writes them back to a coordinator that boots blank again, forever.
            // Refuse to back up a coordinator that is not currently on a network:
            // there is nothing ZBOSS-loadable to capture, and a host that stores
            // such an image gets a backup that can never restore. This runs
            // under the ZBOSS big-lock (the whole command dispatch switch in
            // zb_ncp.cpp holds zboss_lock_guard), so the live getters are safe
            // with no additional lock. Deterministic and format-independent; it
            // does NOT attempt to replicate ZBOSS's internal dataset CRC.
            if (!zb_zdo_joined() || zb_get_pan_id() == 0xFFFF) {
                ESP_LOGE(TAG, "GET_NETWORK_BACKUP REFUSED: not on a network "
                         "(joined=%d panID=0x%04x) -- no loadable state to back up",
                         (int)zb_zdo_joined(), (unsigned)zb_get_pan_id());
                if (s_backup_snapshot) {
                    free(s_backup_snapshot);
                    s_backup_snapshot = nullptr;
                    s_backup_snapshot_size = 0;
                }
                full_res->status = GENERIC_OPERATION_FAILED;
                const uint32_t zero = 0;
                memcpy(payload,     &zero, 4);
                memcpy(payload + 4, &zero, 4);
                return sizeof(generic_response_t) + 8;
            }

            if (s_backup_snapshot && s_backup_snapshot_size != total_size) {
                free(s_backup_snapshot);
                s_backup_snapshot = nullptr;
            }
            if (!s_backup_snapshot) {
                s_backup_snapshot = static_cast<uint8_t*>(malloc(total_size));
            }
            if (s_backup_snapshot) {
                s_backup_snapshot_size = total_size;
                esp_err_t sre = esp_partition_read(nvs_part, 0, s_backup_snapshot, nvs_part->size);
                if (sre == ESP_OK) {
                    sre = esp_partition_read(zb_part, 0, s_backup_snapshot + nvs_part->size, zb_part->size);
                }
                if (sre != ESP_OK) {
                    ESP_LOGE(TAG, "GET_NETWORK_BACKUP: snapshot read failed: %s -- direct reads",
                             esp_err_to_name(sre));
                    free(s_backup_snapshot);
                    s_backup_snapshot = nullptr;
                    s_backup_snapshot_size = 0;
                }
            } else {
                s_backup_snapshot_size = 0;
                ESP_LOGE(TAG, "GET_NETWORK_BACKUP: snapshot alloc failed (%lu B) -- direct reads",
                         (unsigned long)total_size);
            }
            ESP_LOGI(TAG, "GET_NETWORK_BACKUP: session start total=%lu snapshot=%d",
                     (unsigned long)total_size, s_backup_snapshot != nullptr);
        }

        uint32_t len = 128;
        if (offset >= total_size) {
            len = 0;
        } else if (offset + len > total_size) {
            len = total_size - offset;
        }

        memcpy(payload,     &total_size, 4);
        memcpy(payload + 4, &len,        4);

        if (len > 0) {
            ESP_LOGD(TAG, "GET_NETWORK_BACKUP: chunk offset=%lu len=%lu",
                     (unsigned long)offset, (unsigned long)len);
            if (s_backup_snapshot && offset + len <= s_backup_snapshot_size) {
                memcpy(payload + 8, s_backup_snapshot + offset, len);
            } else {
                esp_err_t re = ESP_OK;
                if (offset < nvs_part->size) {
                    uint32_t read_len = len;
                    if (offset + read_len > nvs_part->size) {
                        read_len = nvs_part->size - offset;
                    }
                    re = esp_partition_read(nvs_part, offset, payload + 8, read_len);
                    if (re == ESP_OK && read_len < len) {
                        re = esp_partition_read(zb_part, 0, payload + 8 + read_len, len - read_len);
                    }
                } else {
                    re = esp_partition_read(zb_part, offset - nvs_part->size, payload + 8, len);
                }
                if (re != ESP_OK) {
                    // Same unchecked-esp_partition_* class as M1, on the backup-READ
                    // side: an unchecked failed read would ship stale stack bytes to
                    // the host as a "valid" chunk with status OK -> silently corrupt
                    // coordinator_backup.json. Report failure with len=0 instead,
                    // mirroring the partitions-missing path above.
                    ESP_LOGE(TAG, "GET_NETWORK_BACKUP: read failed at offset %lu: %s",
                             (unsigned long)offset, esp_err_to_name(re));
                    full_res->status = GENERIC_OPERATION_FAILED;
                    len = 0;
                    memcpy(payload + 4, &len, 4);
                    return sizeof(generic_response_t) + 8;
                }
            }
        }

        // Last chunk served (or out-of-range read) — release the snapshot.
        if (offset + len >= total_size && s_backup_snapshot) {
            ESP_LOGI(TAG, "GET_NETWORK_BACKUP: session complete, snapshot freed");
            free(s_backup_snapshot);
            s_backup_snapshot = nullptr;
            s_backup_snapshot_size = 0;
        }

        return sizeof(generic_response_t) + 8 + len;
    }
};

// Feature 4: gross-garbage gate for a restored zb_storage image. HONEST SCOPE --
// catches truncated/zeroed/garbage uploads (all-0xFF, all-0x00, wrong page
// magic) but CANNOT catch peca-class subtle corruption (a structurally valid
// page ZBOSS rejects on its closed-lib per-dataset tail CRC). The page magic was
// reverse-engineered from real C6 images (healthy self vs peca89 poison; both
// NVRAM format version 11) -- we ACCEPT on an unexpected version rather than
// risk false-rejecting a legitimate newer / other-target image.
// Review #5 (doc): the gate validates ONLY the zb_storage half (the ZBOSS
// network NVRAM). A garbage `nvs` half paired with a valid `zb_storage` half
// PASSES and force-reboots. Bounded/self-healing on this USB-only master: the
// nvs partition carries no WiFi creds here, and app.cpp re-inits/erases an
// unmountable nvs on boot. A full nvs validation is deliberately not attempted.
static constexpr uint32_t ZB_NVRAM_PAGE_SZ    = 0x2000;      // 8 KB, two per zb_storage half
static constexpr uint32_t ZB_NVRAM_PAGE_MAGIC = 0x001e0040;  // (+0x04) len=0x40, type=0x1e=PAGE_HDR

// Classify one 0x2000 ZBOSS NVRAM page from a RAM buffer. NO zb_* calls.
//   0 = page header invalid (gross garbage)
//   1 = valid header, no data records (inactive/empty page)
//   2 = valid header AND >=1 well-formed dataset record
static int zb_nvram_page_class(const uint8_t* p) {
    uint32_t magic, v1, v2, seq;
    uint16_t rlen, rtype;
    memcpy(&magic, p + 0x04, 4);   // unaligned-safe
    memcpy(&v1,    p + 0x08, 4);
    memcpy(&v2,    p + 0x10, 4);
    if (magic != ZB_NVRAM_PAGE_MAGIC) return 0;
    if ((v1 != 10 && v1 != 11) || (v2 != 10 && v2 != 11)) {
        ESP_LOGW(TAG, "RESTORE_NETWORK gate: unexpected NVRAM version %lu/%lu (accepting)",
                 (unsigned long)v1, (unsigned long)v2);
    }
    memcpy(&seq,   p + 0x40 + 0, 4);  // first dataset record sits after the 0x40 page header
    memcpy(&rlen,  p + 0x40 + 4, 2);
    memcpy(&rtype, p + 0x40 + 6, 2);
    if (seq == 0xFFFFFFFFu || rlen == 0xFFFF) return 1;   // header OK, no records
    if (rlen == 0 || rlen > ZB_NVRAM_PAGE_SZ) return 0;   // malformed record length
    if (rtype >= 64) return 0;                            // dataset type out of enum range
    return 2;
}

// Gate the assembled 16 KB zb_storage half: accept iff at least one page carries
// a well-formed dataset record (rejects all-0xFF / all-0x00 / wrong-magic).
static bool zb_storage_image_ok(const uint8_t* img, uint32_t size) {
    bool any_record = false;
    for (uint32_t off = 0; off + ZB_NVRAM_PAGE_SZ <= size; off += ZB_NVRAM_PAGE_SZ) {
        if (zb_nvram_page_class(img + off) >= 2) any_record = true;
    }
    return any_record;
}

template <>
struct zb_ncp::cmd_handle<RESTORE_NETWORK> : immediate_cmd_process<RESTORE_NETWORK> {
    // Bug pre-fix: resp_buffer_size was 0 but process_immediate writes a
    // 2-byte generic_response_t — 2-byte stack overflow in the wrapper's
    // outdata buffer. Now sized correctly.
    static constexpr size_t resp_buffer_size = sizeof(generic_response_t);

    static size_t process_immediate(const void *inbuffer, size_t inlen, uint8_t* outdata, size_t outdata_size) {
        if (inlen < 8) {
            ESP_LOGE(TAG, "RESTORE_NETWORK: short frame %d", int(inlen));
            return restore_fail(outdata, GENERIC_INVALID_PARAMETER);
        }

        const uint8_t* in_ptr = static_cast<const uint8_t*>(inbuffer);
        uint32_t offset      = 0;
        uint32_t total_size  = 0;
        memcpy(&offset,     in_ptr,     4);  // unaligned-safe
        memcpy(&total_size, in_ptr + 4, 4);
        const uint32_t chunk_length = inlen - 8;

        const auto nvs_part = backup_nvs_partition();
        const auto zb_part  = backup_zb_partition();
        if (!nvs_part || !zb_part) {
            ESP_LOGE(TAG, "RESTORE_NETWORK: partitions missing (nvs=%p zb=%p)", nvs_part, zb_part);
            return restore_fail(outdata, GENERIC_OPERATION_FAILED);
        }
        const uint32_t capacity = nvs_part->size + zb_part->size;

        // C3: declared total_size from host must match what we can store.
        if (total_size != capacity) {
            ESP_LOGE(TAG, "RESTORE_NETWORK: total_size %lu != capacity %lu",
                     (unsigned long)total_size, (unsigned long)capacity);
            return restore_fail(outdata, GENERIC_INVALID_PARAMETER);
        }
        // C3: chunk must stay strictly inside the image.
        if (offset > capacity || chunk_length > capacity - offset) {
            ESP_LOGE(TAG, "RESTORE_NETWORK: chunk OOB offset=%lu len=%lu cap=%lu",
                     (unsigned long)offset, (unsigned long)chunk_length, (unsigned long)capacity);
            return restore_fail(outdata, GENERIC_INVALID_PARAMETER);
        }

        // C2: session ordering. offset==0 opens (or restarts) a session and
        // performs the destructive erase exactly once per stream. Non-zero
        // offsets must arrive in monotonic order *during* an active session —
        // this also serves as a partial C1 mitigation: a stray RESTORE frame
        // with offset!=0 from a buggy or hostile peer is rejected without
        // touching flash, instead of e.g. silently writing into nvs.
        if (offset == 0) {
            if (s_restore_session.active) {
                ESP_LOGW(TAG, "RESTORE_NETWORK: prior session at %lu/%lu abandoned, restarting",
                         (unsigned long)s_restore_session.next_offset,
                         (unsigned long)s_restore_session.total_size);
            }
            ESP_LOGI(TAG, "RESTORE_NETWORK: begin, total %lu bytes", (unsigned long)total_size);
            restore_free_buf();   // drop any buffer left by an abandoned session
            // F4 buffer-before-commit: accumulate the WHOLE image in RAM and write
            // NOTHING to flash until the final chunk passes the gate -- so a
            // rejected (gross-garbage) restore leaves nvs + zb_storage entirely
            // untouched (no broken NVS, no lost creds, no reboot). On a malloc
            // failure we degrade to the legacy erase-upfront + stream-to-flash
            // path (always reboots, no gate) -- matches the GET snapshot idiom.
            s_restore_img = static_cast<uint8_t*>(malloc(capacity));
            if (s_restore_img) {
                memset(s_restore_img, 0xFF, capacity);  // unfilled regions look erased
                // nvs + zb_storage stay intact on flash for the whole upload.
            } else {
                ESP_LOGW(TAG, "RESTORE_NETWORK: buffer declined (heap), streaming without gate");
                extern esp_err_t nvs_flash_deinit(void);
                nvs_flash_deinit();
                // Flash is about to be erased -- latch dirty so EVERY later exit
                // reboots (review #1), including a failed erase below.
                s_restore_flash_dirty = true;
                esp_err_t er_nvs = esp_partition_erase_range(nvs_part, 0, nvs_part->size);
                esp_err_t er_zb  = esp_partition_erase_range(zb_part,  0, zb_part->size);
                if (er_nvs != ESP_OK || er_zb != ESP_OK) {
                    // M1: a failed erase leaves NVS half-wiped. We have already
                    // de-init'd + (partially) erased nvs, so we can NOT keep running
                    // -- restore_fail() reboots because flash is dirty, landing on a
                    // clean re-mount next boot instead of a half-erased partition.
                    ESP_LOGE(TAG, "RESTORE_NETWORK: erase failed nvs=%s zb=%s",
                             esp_err_to_name(er_nvs), esp_err_to_name(er_zb));
                    return restore_fail(outdata, GENERIC_OPERATION_FAILED);
                }
            }
            s_restore_session.active      = true;
            s_restore_session.total_size  = total_size;
            s_restore_session.next_offset = 0;
        } else {
            if (!s_restore_session.active) {
                ESP_LOGE(TAG, "RESTORE_NETWORK: chunk at offset %lu without active session",
                         (unsigned long)offset);
                return restore_fail(outdata, GENERIC_INVALID_STATE);
            }
            if (offset != s_restore_session.next_offset) {
                ESP_LOGE(TAG, "RESTORE_NETWORK: out-of-order chunk: got %lu, expected %lu",
                         (unsigned long)offset, (unsigned long)s_restore_session.next_offset);
                return restore_fail(outdata, GENERIC_INVALID_PARAMETER);
            }
        }

        // Accumulate (F4 buffering) or, on the legacy low-heap path, stream
        // straight to flash splitting across the nvs/zb_storage boundary.
        if (chunk_length > 0) {
            if (s_restore_img) {
                // F4 buffering: just accumulate; flash is written only at commit.
                memcpy(s_restore_img + offset, in_ptr + 8, chunk_length);
            } else {
                esp_err_t we = ESP_OK;
                if (offset < nvs_part->size) {
                    uint32_t write_len = chunk_length;
                    if (offset + write_len > nvs_part->size) {
                        write_len = nvs_part->size - offset;
                    }
                    we = esp_partition_write(nvs_part, offset, in_ptr + 8, write_len);
                    if (we == ESP_OK && write_len < chunk_length) {
                        we = esp_partition_write(zb_part, 0, in_ptr + 8 + write_len, chunk_length - write_len);
                    }
                } else {
                    we = esp_partition_write(zb_part, offset - nvs_part->size, in_ptr + 8, chunk_length);
                }
                if (we != ESP_OK) {
                    // M1: bail on a failed flash write instead of marching to the
                    // completion reboot with a corrupt image. Flash is already
                    // erased+partially-written this boot, so restore_fail() reboots
                    // (review #1) -- a clean re-mount beats running on a torn nvs.
                    ESP_LOGE(TAG, "RESTORE_NETWORK: write failed at offset %lu: %s",
                             (unsigned long)offset, esp_err_to_name(we));
                    return restore_fail(outdata, GENERIC_OPERATION_FAILED);
                }
            }
        }

        s_restore_session.next_offset = offset + chunk_length;

        if (s_restore_session.next_offset >= s_restore_session.total_size) {
            // Mark inactive so a duplicate final chunk (host retry) does not
            // re-run the gate / spawn a second reboot task.
            s_restore_session.active = false;
            // F4: gate the buffered image before committing ANYTHING to flash.
            // Gross garbage (all-FF/all-00/wrong-magic zb_storage) -> reject with
            // NOTHING written: nvs + zb_storage + WiFi creds all survive untouched,
            // no reboot, the host learns it failed. HONEST: a peca-class subtle
            // corruption PASSES this gate (it is structurally a real network) --
            // only Feature 1 (silent-wipe detection) catches that class.
            if (s_restore_img) {
                if (!zb_storage_image_ok(s_restore_img + nvs_part->size, zb_part->size)) {
                    ESP_LOGE(TAG, "RESTORE_NETWORK: image failed gross-garbage gate "
                                  "-- nothing written, prior state preserved");
                    // restore_fail() reboots ONLY if a prior (legacy) session
                    // already dirtied flash this boot (review #1); a pure buffered
                    // reject never touched flash -> side-effect-free, no reboot.
                    return restore_fail(outdata, GENERIC_OPERATION_FAILED);
                }
                // Gate passed: commit BOTH halves. Erasing irreversibly destroys
                // the old network, so from here flash is dirty and we ALWAYS reboot
                // (NVS is deinit'd; app::init re-mounts it next boot).
                extern esp_err_t nvs_flash_deinit(void);
                nvs_flash_deinit();
                s_restore_flash_dirty = true;
                esp_err_t e1 = esp_partition_erase_range(nvs_part, 0, nvs_part->size);
                esp_err_t e2 = esp_partition_erase_range(zb_part,  0, zb_part->size);
                esp_err_t e3 = (e1 == ESP_OK)
                    ? esp_partition_write(nvs_part, 0, s_restore_img, nvs_part->size) : e1;
                esp_err_t e4 = (e2 == ESP_OK)
                    ? esp_partition_write(zb_part, 0, s_restore_img + nvs_part->size, zb_part->size) : e2;
                if (e3 != ESP_OK || e4 != ESP_OK) {
                    // Review #2: a partial write must NOT persist a truncated/torn
                    // zb_storage page for ZBOSS to choke on (the peca corruption
                    // class). The old network is already gone (we erased it), so
                    // make the forced reboot land CLEAN: wipe zb_storage so ZBOSS
                    // boots factory-new instead of half-written. Free the buffer
                    // AFTER the result check (so it was available for a retry had
                    // one been added) -- review #2.
                    ESP_LOGE(TAG, "RESTORE_NETWORK: commit failed nvs=%s zb=%s "
                                  "-- erasing zb_storage to clean-blank, then reboot",
                             esp_err_to_name(e3), esp_err_to_name(e4));
                    esp_partition_erase_range(zb_part, 0, zb_part->size);
                    restore_free_buf();
                    restore_schedule_reboot();
                    return restore_reply(outdata, GENERIC_OPERATION_FAILED);
                }
                restore_free_buf();
            }
            ESP_LOGI(TAG, "RESTORE_NETWORK: complete, rebooting in 1s");
            restore_schedule_reboot();
        }

        return restore_reply(outdata, GENERIC_OK);
    }
};


// =====================================================================
// Structured backup — GET_STRUCTURED_BACKUP (0x009B) / RESTORE_STRUCTURED_BACKUP (0x009C)
//
// Replaces the brittle raw-NVS dump (0x0099/0x009A) with a small TLV image
// that carries the semantically meaningful network identity (PAN, ExtPAN,
// channel, updateId, IEEE, NWK key, NWK outgoing frame counter) plus an
// informational device-table snapshot. Wire layout in backup_structured.h.
//
// Limitations:
//   - TC link key is setter-only in esp-zigbee-lib, so backups do not carry
//     it; RESTORE leaves the default ZigBeeAlliance09 TC key in place.
//     Devices joined with a custom-TC key will need re-pair.
//   - NWK key sequence number has no public getter; backups omit it and
//     RESTORE installs the network key at the stack default seq (devices
//     re-sync within one Network-Status round-trip in practice).
//   - APS per-peer frame counters are not exported either; APS-level strict
//     replay enforcers may briefly drop frames after restore until they
//     re-sync via the standard rejoin path.
//   - TAG_DEVICE_TABLE is informational (host-side only). RESTORE ignores
//     it — the firmware does not pre-populate the neighbor table; existing
//     devices rejoin via Device_annce once the network is back on air.
// =====================================================================

#include "backup_structured.h"
#include "nwk/esp_zigbee_nwk.h"
#include "esp_zigbee_secur.h"

template <>
struct zb_ncp::cmd_handle<GET_STRUCTURED_BACKUP> : immediate_cmd_process<GET_STRUCTURED_BACKUP> {
    static constexpr size_t resp_buffer_size =
        sizeof(generic_response_t) + backup_structured::MAX_IMAGE_SIZE;

    static size_t process_immediate(const void *inbuffer, size_t inlen,
                                    uint8_t* outdata, size_t outdata_size) {
        auto status = reinterpret_cast<generic_response_t*>(outdata);
        status->category = STATUS_CATEGORY_GENERIC;
        status->status   = GENERIC_OK;

        uint8_t* image_start = outdata + sizeof(generic_response_t);
        uint8_t* cursor      = image_start;

        // Reserve room for the header — patched up at the end with payload_len.
        auto hdr = reinterpret_cast<backup_structured::header_t*>(cursor);
        memcpy(hdr->magic, backup_structured::MAGIC, 4);
        hdr->version = backup_structured::VERSION;
        hdr->flags   = 0;
        cursor += sizeof(backup_structured::header_t);
        uint8_t* tlv_start = cursor;

        // Emits one TLV: tag(1) + len(2 LE) + value(len). Increments cursor.
        auto emit = [&](uint8_t tag, const void* value, uint16_t len) {
            *cursor++ = tag;
            memcpy(cursor, &len, 2);  cursor += 2;
            memcpy(cursor, value, len); cursor += len;
        };

        uint16_t pan = esp_zb_get_pan_id();
        emit(backup_structured::TAG_PAN_ID, &pan, 2);

        esp_zb_ieee_addr_t ext_pan;
        esp_zb_get_extended_pan_id(ext_pan);
        emit(backup_structured::TAG_EXT_PAN_ID, ext_pan, 8);

        uint8_t chan = esp_zb_get_current_channel();
        emit(backup_structured::TAG_CHANNEL, &chan, 1);

        uint8_t upd = esp_zb_nwk_get_update_id();
        emit(backup_structured::TAG_NWK_UPDATE_ID, &upd, 1);

        esp_zb_ieee_addr_t ieee;
        esp_zb_get_long_address(ieee);
        emit(backup_structured::TAG_COORD_IEEE, ieee, 8);

        // BACKUP-5 (by design, decided 2026-05-30): the plaintext network key is
        // exported over the unauthenticated USB link. This is intentional parity
        // with mainline Z2M, whose coordinator_backup.json (Universal NWK Backup
        // format) already stores networkKey in cleartext on the host filesystem —
        // so this exposes nothing the host doesn't already persist. A host-set
        // "lock" gate was considered and rejected: the shipped Z2M fork calls
        // backup() automatically (controller start/stop + daily) and sends no
        // unlock, so a default-locked gate would silently write keyless,
        // unrestorable backups. The only threat a lock would add cover for is a
        // passive USB-link sniffer that cannot read the host's data/ dir — too
        // narrow to justify the regression. Leave exported; revisit only on
        // concrete demand (then a default-UNLOCKED opt-in lock + a matching
        // patch_zboss.js change that sends the unlock before backup()).
        uint8_t nwk_key[16];
        if (esp_zb_secur_primary_network_key_get(nwk_key) == ESP_OK) {
            emit(backup_structured::TAG_NWK_KEY, nwk_key, 16);
        } else {
            ESP_LOGW(TAG, "GET_STRUCTURED_BACKUP: NWK key not available (not joined?)");
        }

        uint32_t fc = esp_zb_nwk_get_frame_counter();
        emit(backup_structured::TAG_NWK_FRAME_COUNTER, &fc, 4);

        // Device table: open a TLV with placeholder len, iterate neighbors,
        // patch the length once the count is known.
        uint8_t* dev_tlv = cursor;
        *cursor++ = backup_structured::TAG_DEVICE_TABLE;
        uint8_t* dev_len_field = cursor;
        cursor += 2;

        esp_zb_nwk_info_iterator_t iter = ESP_ZB_NWK_INFO_ITERATOR_INIT;
        esp_zb_nwk_neighbor_info_t nbr;
        size_t dev_count = 0;
        while (dev_count < backup_structured::MAX_DEVICES &&
               esp_zb_nwk_get_next_neighbor(&iter, &nbr) == ESP_OK) {
            auto rec = reinterpret_cast<backup_structured::device_record_t*>(cursor);
            memcpy(rec->ieee, nbr.ieee_addr, 8);
            rec->short_addr      = nbr.short_addr;
            rec->rx_on_when_idle = nbr.rx_on_when_idle;
            rec->relationship    = nbr.relationship;
            rec->device_type     = nbr.device_type;
            rec->depth           = nbr.depth;
            rec->lqi             = nbr.lqi;
            rec->reserved        = 0;
            cursor += sizeof(backup_structured::device_record_t);
            ++dev_count;
        }
        uint16_t dev_len = dev_count * sizeof(backup_structured::device_record_t);
        memcpy(dev_len_field, &dev_len, 2);
        (void)dev_tlv;

        hdr->payload_len = cursor - tlv_start;
        ESP_LOGI(TAG, "GET_STRUCTURED_BACKUP: %u devices, image %u bytes",
                 (unsigned)dev_count, (unsigned)(cursor - image_start));

        return cursor - outdata;
    }
};


// RESTORE writes the validated TLV image to NVS under "restore_pend"/"tlv",
// erases the ZBOSS storage partition so the stack does not pre-load the old
// network on next boot, and schedules a reboot. The actual application of
// settings happens in zb_ncp::apply_pending_restore() right after ZB_INIT in
// init_int(), where the esp_zb_set_* / esp_zb_secur_* APIs are in their
// legal "not yet joined" state.
template <>
struct zb_ncp::cmd_handle<RESTORE_STRUCTURED_BACKUP> : immediate_cmd_process<RESTORE_STRUCTURED_BACKUP> {
    static constexpr size_t resp_buffer_size = sizeof(generic_response_t);

    static size_t reply(uint8_t* outdata, ncp_generic_status_t s) {
        auto r = reinterpret_cast<generic_response_t*>(outdata);
        r->category = STATUS_CATEGORY_GENERIC;
        r->status   = s;
        return sizeof(generic_response_t);
    }

    static size_t process_immediate(const void *inbuffer, size_t inlen,
                                    uint8_t* outdata, size_t outdata_size) {
        if (inlen < sizeof(backup_structured::header_t)) {
            ESP_LOGE(TAG, "RESTORE_STRUCTURED_BACKUP: short frame %u", (unsigned)inlen);
            return reply(outdata, GENERIC_INVALID_PARAMETER);
        }
        auto in = static_cast<const uint8_t*>(inbuffer);
        backup_structured::header_t hdr;
        memcpy(&hdr, in, sizeof(hdr));  // unaligned-safe
        if (memcmp(hdr.magic, backup_structured::MAGIC, 4) != 0) {
            ESP_LOGE(TAG, "RESTORE_STRUCTURED_BACKUP: bad magic");
            return reply(outdata, GENERIC_INVALID_PARAMETER);
        }
        if (hdr.version != backup_structured::VERSION) {
            ESP_LOGE(TAG, "RESTORE_STRUCTURED_BACKUP: version %u, expected %u",
                     hdr.version, backup_structured::VERSION);
            return reply(outdata, GENERIC_INVALID_PARAMETER);
        }
        if ((size_t)sizeof(hdr) + hdr.payload_len > inlen) {
            ESP_LOGE(TAG, "RESTORE_STRUCTURED_BACKUP: declared %u + hdr > inlen %u",
                     hdr.payload_len, (unsigned)inlen);
            return reply(outdata, GENERIC_INVALID_PARAMETER);
        }

        nvs_handle_t nh;
        esp_err_t err = nvs_open("restore_pend", NVS_READWRITE, &nh);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RESTORE_STRUCTURED_BACKUP: nvs_open failed: %s",
                     esp_err_to_name(err));
            return reply(outdata, GENERIC_OPERATION_FAILED);
        }
        err = nvs_set_blob(nh, "tlv", inbuffer, inlen);
        if (err == ESP_OK) err = nvs_commit(nh);
        nvs_close(nh);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RESTORE_STRUCTURED_BACKUP: nvs_set_blob/commit failed: %s",
                     esp_err_to_name(err));
            return reply(outdata, GENERIC_OPERATION_FAILED);
        }

        // Wipe the ZBOSS storage so the stack does a fresh init on next boot
        // — the apply_pending_restore() hook will then plant the saved
        // settings before the stack persists them via its normal save path.
        const auto zb_part = backup_zb_partition();
        if (zb_part) {
            esp_err_t ze = esp_partition_erase_range(zb_part, 0, zb_part->size);
            if (ze != ESP_OK) {
                // M1: don't reboot into apply_pending_restore() on a failed wipe
                // — the stack would re-persist over a stale zb_storage. Report
                // failure; the host can retry the whole RESTORE_STRUCTURED.
                ESP_LOGE(TAG, "RESTORE_STRUCTURED_BACKUP: zb erase failed: %s",
                         esp_err_to_name(ze));
                return reply(outdata, GENERIC_OPERATION_FAILED);
            }
        } else {
            ESP_LOGW(TAG, "RESTORE_STRUCTURED_BACKUP: zb_storage partition missing");
        }

        ESP_LOGI(TAG, "RESTORE_STRUCTURED_BACKUP: %u-byte image stored, rebooting in 1s",
                 (unsigned)inlen);
        xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(1000)); esp_restart(); },
                    "rb_struct", 2048, NULL, 5, NULL);

        return reply(outdata, GENERIC_OK);
    }
};
