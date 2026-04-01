#include "app.h"
#include "transport.h"
#include "protocol.h"
#include "zb_ncp.h"
#include <nvs_flash.h>
#include <esp_log.h>

static const char* TAG = "APP";

app::app() {

}

app& app::instance() {
	static app s_app;
	return s_app;
}

void app::on_rx_data(const void* data, size_t size) {
	zb_ncp::on_rx_data(data,size);
}

esp_err_t app::send_event_int(const ctx_t& ctx) {
	//ESP_LOGI(TAG,"send event %d %d",ctx.event,ctx.size);
	BaseType_t ret = pdTRUE;
    if (xPortInIsrContext() == pdTRUE) {
        ret = xQueueSendFromISR(m_queue, &ctx, NULL);
    } else {
        ret = xQueueSend(m_queue, &ctx, 0);
    }
	
    return (ret == pdTRUE) ? ESP_OK : ESP_FAIL ;
}

esp_err_t app::process_event(const ctx_t& ctx) {
	size_t recv_size = 0;
    esp_err_t ret = ESP_OK;

    if (ctx.size > sizeof(m_buffer)) {
        ESP_LOGE(TAG, "Process event out of memory %d",ctx.size);
        return ESP_ERR_NO_MEM;
    }

    switch (ctx.event) {
        case EVENT_INPUT:
        	ret = transport::process_input(m_buffer,ctx.size);
            break;
        case EVENT_OUTPUT:
            recv_size = transport::output_receive(m_buffer,ctx.size);
			if (recv_size != ctx.size) {
                ESP_LOGE(TAG, "Output buffer receive error: size %d expect %d!", recv_size, ctx.size);
            } else {
                //ESP_LOGD(TAG,"esp_ncp_bus_output %d",ctx->size);
                ret = protocol::on_rx(m_buffer, ctx.size);
            }
            break;
        case EVENT_RESET:
            esp_restart();
            break;
        default:
            break;
    }

    return ret;
}

esp_err_t app::init_int() {
	ESP_LOGI(TAG,"init_int");

	auto res = protocol::init();
	if (res != ESP_OK)
		return res;
	res = transport::init();
	if (res != ESP_OK)
		return res;
	m_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(ctx_t));
	if (!m_queue) {
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

esp_err_t app::start_int() {
	ESP_LOGV(TAG,"start_int");
	auto res = transport::start();
	if (res != ESP_OK)
		return res;

    // https://github.com/andryblack/esp-coordinator/issues/11 - Add an NCP reset response after firmware startup #11
    uint8_t raw_data[] = {0xDE, 0xAD, 0x05, 0x00, 0x06, 0x01, 0x8F}; //ack
    uint8_t raw_data1[] = {0xDE, 0xAD, 0x0E, 0x00, 0x06, 0xC0, 0x5D, 0x50, 0xD4, 0x00, 0x01, 0x02, 0x00, 0xFF, 0x00, 0x00}; //ответ
    transport::send(raw_data, sizeof(raw_data));
    transport::send(raw_data1, sizeof(raw_data1));

 	// uint8_t outdata[2] = { 0,0 };
    // esp_ncp_header_t header = {{0,0,0,},NCP_RESET,0,0};
    // esp_ncp_resp_input(&header, outdata, sizeof(outdata)); 

	ctx_t ctx;
    while (true) {
        if (xQueueReceive(m_queue, &ctx, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (process_event(ctx) != ESP_OK) {
            ESP_LOGE(TAG, "Process event fail");
            break;
        }
    }
	return ESP_OK;
}

esp_err_t app::init() {
	ESP_LOGI(TAG,"init");
	auto res = nvs_flash_init();
	if (res != ESP_OK)
		return res;
	res = zb_ncp::init();
	if (res != ESP_OK) {
		return res;
	}
	return instance().init_int();
}


esp_err_t app::start() {
	ESP_LOGI(TAG,"start");
	return instance().start_int();;
}