#pragma once
#include "zboss_decl.h"
#include "commands.h"

extern "C" void zboss_signal_handler(zb_uint8_t param);

class zb_ncp {
public:
	enum frame_type_t : uint8_t {
		REQUEST = 0,
	    RESPONSE = 1,
	    INDICATION = 2,
	};
	struct cmd_t {
		uint8_t version;
		frame_type_t type;
		command_id_t command_id;
		uint8_t tsn;
	} __attribute__((packed));
	static constexpr size_t MAX_PARALLEL_REQUESTS = 16;
	static constexpr size_t ZB_TASK_STACK_SIZE = 1024 * 8;
	// Overall network size the coordinator is configured for; the address /
	// child tables are sized to this in init_int() (zb_config_overall_network_size
	// + zb_set_max_children). GETSET-2: SET_MAX_CHILDREN rejects host values
	// above it — they can't be honored and oversizing children feeds the
	// zb_address.c:395 table-exhaustion assertion family.
	static constexpr uint16_t OVERALL_NETWORK_SIZE = 200;
private:
	template <command_id_t Cmd>
	struct cmd_handle;
	template <command_id_t CmdId>
	struct immediate_cmd_process;
	template <command_id_t CmdId,template<command_id_t> typename ResolveStrategyT>
	struct delayed_cmd_process;
	template <command_id_t CmdId,typename Arg, typename Req, typename Resp>
	struct request_cmd_process;
	template <command_id_t CmdId,typename Res>
	struct general_status_res;
	template <command_id_t CmdId,typename Arg>
	struct general_status_arg;
	template <command_id_t CmdId,typename Arg,typename Res>
	struct general_status_arg_res;
	
	friend void zboss_signal_handler(zb_uint8_t param);

	uint32_t m_channels_mask;
	static void continue_zboss(uint8_t );
	static void set_channel_mask(uint32_t mask);
	static void ncp_zb_task(void* arg);

private:
	zb_ncp();
	static zb_ncp& instance();
	esp_err_t init_int();
public:
	static esp_err_t init() { return instance().init_int(); }
	// Boot-time entry: start the ZBOSS dispatch task so the persisted NVRAM
	// dataset is loaded and SKIP_STARTUP fires (which then runs continue_zboss
	// → boot-ready NCP_RESET response + bdb_start_top_level_commissioning).
	// Without this the task is only started lazily from NWK_FORMATION /
	// NWK_START_WITHOUT_FORMATION command handlers — but z2m never sends those
	// at startup, it queries GET_JOINED / GET_PAN_ID first and on the stale
	// 0xFFFF/0xFF defaults decides to formNetwork() and wipes paired devices.
	// See andryblack/esp-coordinator#5 / #19, z2m #26152. Idempotent.
	static bool start_zigbee_stack();
	// The synthetic boot-ready NCP_RESET response (tsn=0xFF). Normally sent
	// from continue_zboss once the NVRAM dataset is loaded; in boot-guard
	// safe mode app::start_int sends it instead (ZBOSS never starts there),
	// so hosts proceed to their first getters and fail VISIBLY on
	// GENERIC_BLOCKED rather than timing out on a silent port.
	static void send_boot_ready_frame();
	static void on_rx_data(const void* data,size_t size);
	static void indication(command_id_t cmd,const void* data,size_t size);
	static void send_cmd_data(const void* data,size_t size);

	// Intercept hook for malformed ZDP Simple_Desc_rsp frames. The ZBOSS
	// stack parser (libzboss_stack.zczr.a) does a strict
	//     frame_length == declared_simple_desc_length
	// check and drops responses with trailing extra bytes — see Espressif
	// issue esp-zigbee-sdk#485, confirmed by @xieqinan 2024-11-19. Some Tuya
	// devices (OUI prefix 0x70b3d5...) emit Simple_Desc_rsp with 2 trailing
	// bytes, so ZBOSS never invokes the application callback for them; the
	// host hits a 10 s timeout, ZBOSS later returns ZB_ZDP_STATUS_TIMEOUT
	// after ~20 s of APS retries, interview fails permanently.
	//
	// This static method parses the response tolerantly (uses byte counts,
	// ignores trailing junk past declared length), synthesises the wire
	// payload exactly as cmd_handle<ZDO_SIMPLE_DESC_REQ>::format_response
	// would have produced, and dispatches it via send_cmd_data. The matching
	// pending request slot is marked S_NONE so the late TIMEOUT callback
	// from ZBOSS finds no match and is silently dropped. Returns true if a
	// pending request was matched and serviced.
	static bool try_intercept_simple_desc_rsp(const uint8_t* payload, uint16_t len);
};