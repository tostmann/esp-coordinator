#include "esp_partition.h"
#include "nvs_flash.h"
#include "app.h"
#include "version.h"

#include "commands_helpers.h"
#include <esp_mac.h>


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

template <>
struct zb_ncp::cmd_handle<NCP_RESET> : immediate_cmd_process<NCP_RESET>,
		general_status_arg<NCP_RESET,uint8_t> {
	static void process_status_arg(ncp_generic_status_t& status, uint8_t options) {
    	if (options == 1 || options == 2) {
            ESP_LOGI(TAG,"erase nvram");
            zb_nvram_erase();
        } 
        if (options == 2) {
            ESP_LOGI(TAG,"factory reset");
            zb_bdb_reset_via_local_action(0);
        }
        ESP_LOGI(TAG,"restart");
        app::ctx_t ctx = {app::EVENT_RESET, 0};
        app::send_event(ctx);
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
            auto ret = esp_read_mac(res->ieee,ESP_MAC_IEEE802154);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG,"failed read mac addres: %d",ret);
                status = GENERIC_ERROR;
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
		zb_set_tx_power(arg);
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
// struct GET_NWK_KEYS_resp_t {
// 	uint8_t nwkKey1[16];
// 	uint8_t index1;
// 	uint8_t nwkKey2[16];
// 	uint8_t index2;
// 	uint8_t nwkKey3[16];
// 	uint8_t index3;
// } __attribute__((packed));
// template <>
// struct zb_ncp::cmd_handle<GET_NWK_KEYS> : immediate_cmd_process<GET_NWK_KEYS>,
// 		general_status_res<GET_NWK_KEYS,GET_NWK_KEYS_resp_t> {
// 	static void process_status_res(ncp_generic_status_t& status, GET_NWK_KEYS_resp_t* res) {
		
//     }
// };

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
		zb_set_extended_pan_id(arg);
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
    static constexpr size_t resp_buffer_size = 4;
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
            auto channel_mask = *reinterpret_cast<const uint32_t*>(indata);
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
        uint16_t distribNwk = *reinterpret_cast<const uint16_t*>(indata);
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
        if (status == 0) {
            *reinterpret_cast<uint16_t*>(outdata) = zb_get_pan_id();
        } else {
            *reinterpret_cast<uint16_t*>(outdata) = 0;
        }
        return 4;
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
        *reinterpret_cast<uint16_t*>(&outdata[3+ep_count]) = resp->nwk_addr;
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
    	*reinterpret_cast<uint16_t*>(out) = resp->simple_desc.app_profile_id; out+=2;
    	*reinterpret_cast<uint16_t*>(out) = resp->simple_desc.app_device_id; out+=2;
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
    		*reinterpret_cast<uint16_t*>(out) = resp->simple_desc.app_cluster_list[i];
    		out+=2;
    	}
    	for (uint16_t i=0;i<output_count;++i) {
    		*reinterpret_cast<uint16_t*>(out) = resp->simple_desc.app_cluster_list[i+resp->simple_desc.app_input_cluster_count];
    		out+=2;
    	}
    	*reinterpret_cast<uint16_t*>(out) = resp->hdr.nwk_addr; out+=2;

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
        *reinterpret_cast<uint16_t*>(&outdata[2+sizeof(zb_af_node_desc_t)]) = resp->hdr.nwk_addr;
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
        		auto nwks = reinterpret_cast<const uint16_t*>(ext2+1);
        		// MUST use capped `num`, not ext->num_assoc_dev — the latter is
        		// the untruncated remote-reported count; iterating past `num`
        		// overflows the response stack buffer (additional_buffer_size = 2+16*2).
        		for (uint8_t i=0;i<num;++i) {
        			*reinterpret_cast<uint16_t*>(dst) = *nwks++;
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
        		auto nwks = reinterpret_cast<const uint16_t*>(ext2+1);
        		// MUST use capped `num`, see sibling handler ZDO_IEEE_ADDR_REQ.
        		for (uint8_t i=0;i<num;++i) {
        			*reinterpret_cast<uint16_t*>(dst) = *nwks++;
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
        *reinterpret_cast<uint16_t*>(&outdata[2+sizeof(zb_af_node_power_desc_t)]) = resp->hdr.nwk_addr;
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
        *reinterpret_cast<uint16_t*>(out) = resp->nwk_addr; out += 2;
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
template<>
struct zb_ncp::cmd_handle<ZDO_MGMT_LEAVE_REQ> : request_cmd_process< ZDO_MGMT_LEAVE_REQ, ZDO_MGMT_LEAVE_REQ_args_t, zb_zdo_mgmt_leave_param_t, zb_zdo_mgmt_leave_res_t > {
    using Base = request_cmd_process< ZDO_MGMT_LEAVE_REQ, ZDO_MGMT_LEAVE_REQ_args_t, zb_zdo_mgmt_leave_param_t, zb_zdo_mgmt_leave_res_t >;
    static constexpr size_t additional_buffer_size = 0;
    static constexpr const char* name = "ZDO_MGMT_LEAVE_REQ";
    static void format_request(zb_zdo_mgmt_leave_param_t& req, const ZDO_MGMT_LEAVE_REQ_args_t& arg) {
        memset(&req,0,sizeof(zb_zdo_mgmt_leave_param_t));
        req.dst_addr = arg.short_addr;
        memcpy(req.device_address,arg.long_addr,8);
        req.rejoin = (arg.flags & 0x80) ? 1 : 0;
    }
    static uint8_t start_request(uint8_t buf) {
        //ESP_LOGI(TAG,"S_ZDO_MGMT_LEAVE_REQ start_request device_address: " IEEE_ADDR_FMT " dst_addr:%04x ",IEEE_ADDR_PRINT(s_req.device_address),s_req.dst_addr);
        return zdo_mgmt_leave_req(buf,&Base::req_cb);
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
        auto outwrite = &outdata[len];
        auto src = reinterpret_cast<const zb_zdo_binding_table_record_t*>(resp+1);
        auto cnt = resp->binding_table_list_count;
        if (cnt > 64) {
        	ESP_LOGE(TAG,"Truncate ZDO_MGMT_BIND_REQ %d",int(cnt));
        	cnt = 64;
        	reinterpret_cast<zb_zdo_mgmt_bind_resp_t*>(outdata)->binding_table_list_count = cnt;
        }
        for (uint8_t i=0;i<cnt;++i) {
            memcpy(outwrite,src,sizeof(zb_zdo_binding_table_record_t));
            outwrite+=8+1+2+1;
            if (src->dst_addr_mode == 0x01) {
            	outwrite += 2; // short addr
            } else if (src->dst_addr_mode == 0x03) {
            	outwrite += 8 + 1; // long and ep
            }
            ++src;
            
        }
        return outwrite-outdata;
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
    	uint8_t outdata[sizeof(zb_ncp::cmd_t)+8+1+1+4+1];
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
        *reinterpret_cast<uint32_t*>(out) = resp->tx_time; out += 4;
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

            auto req = ResolveStrategy::resolve(data_ptr[1]);
            if (req) {
            	ESP_LOGD(TAG,"%s::aps_user_payload_callback %d",Cmd::name,int(data_ptr[1]));
	        	if (ind->status == 0) {
	        		 Cmd::handle_response(*req,ind);
	        	} else {
	        		report_failed(req->cmd,ind->status);
	        	}
	        	req->state = ResolveStrategy::request_t::S_NONE;
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
    	

    	req.state = ResolveStrategy::request_t::S_EXEC;
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
    		req.state = ResolveStrategy::request_t::S_NONE;
    		report_failed(req.cmd,ret);
    		return;
    	}

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

        uint32_t len = 128;
        if (offset >= total_size) {
            len = 0;
        } else if (offset + len > total_size) {
            len = total_size - offset;
        }

        memcpy(payload,     &total_size, 4);
        memcpy(payload + 4, &len,        4);

        if (len > 0) {
            if (offset < nvs_part->size) {
                uint32_t read_len = len;
                if (offset + read_len > nvs_part->size) {
                    read_len = nvs_part->size - offset;
                }
                esp_partition_read(nvs_part, offset, payload + 8, read_len);
                if (read_len < len) {
                    esp_partition_read(zb_part, 0, payload + 8 + read_len, len - read_len);
                }
            } else {
                esp_partition_read(zb_part, offset - nvs_part->size, payload + 8, len);
            }
        }

        return sizeof(generic_response_t) + 8 + len;
    }
};

template <>
struct zb_ncp::cmd_handle<RESTORE_NETWORK> : immediate_cmd_process<RESTORE_NETWORK> {
    // Bug pre-fix: resp_buffer_size was 0 but process_immediate writes a
    // 2-byte generic_response_t — 2-byte stack overflow in the wrapper's
    // outdata buffer. Now sized correctly.
    static constexpr size_t resp_buffer_size = sizeof(generic_response_t);

    static size_t reply(uint8_t* outdata, ncp_generic_status_t status) {
        auto r = reinterpret_cast<generic_response_t*>(outdata);
        r->category = STATUS_CATEGORY_GENERIC;
        r->status   = status;
        return sizeof(generic_response_t);
    }

    static size_t process_immediate(const void *inbuffer, size_t inlen, uint8_t* outdata, size_t outdata_size) {
        if (inlen < 8) {
            ESP_LOGE(TAG, "RESTORE_NETWORK: short frame %d", int(inlen));
            return reply(outdata, GENERIC_INVALID_PARAMETER);
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
            return reply(outdata, GENERIC_OPERATION_FAILED);
        }
        const uint32_t capacity = nvs_part->size + zb_part->size;

        // C3: declared total_size from host must match what we can store.
        if (total_size != capacity) {
            ESP_LOGE(TAG, "RESTORE_NETWORK: total_size %lu != capacity %lu",
                     (unsigned long)total_size, (unsigned long)capacity);
            return reply(outdata, GENERIC_INVALID_PARAMETER);
        }
        // C3: chunk must stay strictly inside the image.
        if (offset > capacity || chunk_length > capacity - offset) {
            ESP_LOGE(TAG, "RESTORE_NETWORK: chunk OOB offset=%lu len=%lu cap=%lu",
                     (unsigned long)offset, (unsigned long)chunk_length, (unsigned long)capacity);
            return reply(outdata, GENERIC_INVALID_PARAMETER);
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
            extern esp_err_t nvs_flash_deinit(void);
            nvs_flash_deinit();
            esp_partition_erase_range(nvs_part, 0, nvs_part->size);
            esp_partition_erase_range(zb_part,  0, zb_part->size);
            s_restore_session.active      = true;
            s_restore_session.total_size  = total_size;
            s_restore_session.next_offset = 0;
        } else {
            if (!s_restore_session.active) {
                ESP_LOGE(TAG, "RESTORE_NETWORK: chunk at offset %lu without active session",
                         (unsigned long)offset);
                return reply(outdata, GENERIC_INVALID_STATE);
            }
            if (offset != s_restore_session.next_offset) {
                ESP_LOGE(TAG, "RESTORE_NETWORK: out-of-order chunk: got %lu, expected %lu",
                         (unsigned long)offset, (unsigned long)s_restore_session.next_offset);
                return reply(outdata, GENERIC_INVALID_PARAMETER);
            }
        }

        // Split the chunk across the nvs/zb_storage boundary if it straddles.
        if (chunk_length > 0) {
            if (offset < nvs_part->size) {
                uint32_t write_len = chunk_length;
                if (offset + write_len > nvs_part->size) {
                    write_len = nvs_part->size - offset;
                }
                esp_partition_write(nvs_part, offset, in_ptr + 8, write_len);
                if (write_len < chunk_length) {
                    esp_partition_write(zb_part, 0, in_ptr + 8 + write_len, chunk_length - write_len);
                }
            } else {
                esp_partition_write(zb_part, offset - nvs_part->size, in_ptr + 8, chunk_length);
            }
        }

        s_restore_session.next_offset = offset + chunk_length;

        if (s_restore_session.next_offset >= s_restore_session.total_size) {
            ESP_LOGI(TAG, "RESTORE_NETWORK: complete, rebooting in 1s");
            // Mark inactive so a duplicate final chunk (e.g. host retry) does
            // not spawn a second reboot task.
            s_restore_session.active = false;
            xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(1000)); esp_restart(); },
                        "reboot", 2048, NULL, 5, NULL);
        }

        return reply(outdata, GENERIC_OK);
    }
};

