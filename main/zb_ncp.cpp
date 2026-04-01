#include "zb_ncp.h"
#include "commands.h"
#include "protocol.h"
#include "statuses.h"
#include "utils.h"

static const char* TAG = "NCP";

#include "commands_impl.h"

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


esp_err_t zb_ncp::init_int() {
	ZB_INIT();
	zb_set_max_children(64);
    zb_set_nvram_erase_at_start(0);
    zb_set_installcode_policy(0);
    zb_tc_set_use_installcode(0);
    //zgp_disable();

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

void zb_ncp::on_rx_data(const void* data,size_t size) {
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
	uint8_t buffer[256];
	if ((size+4) > sizeof(buffer)) {
		ESP_LOGE(TAG,"Indication too long");
		return;
	}
	buffer[0] = 0;
	buffer[1] = INDICATION;
	*reinterpret_cast<uint16_t*>(&buffer[2]) = cmd;
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
        *reinterpret_cast<uint16_t*>(out) = val;
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
    zb_af_set_data_indication(data_indication);
    
    

   
    ESP_LOGI(TAG,"continue_zboss 1");
    // ncp_cmd_handle<S_ESP_NCP_NETWORK_INIT>::response(0);

    if (cmd_handle<NWK_FORMATION>::need_resolve()) {
        set_channel_mask(instance().m_channels_mask);
        auto res = bdb_start_top_level_commissioning(ZB_BDB_NETWORK_FORMATION);
        if (!res) {
            cmd_handle<NWK_FORMATION>::response(GENERIC_ERROR);
        }
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
