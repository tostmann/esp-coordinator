#include "app.h"
#include "transport.h"
#include "protocol.h"
#include "zb_ncp.h"
#include <nvs_flash.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
	// H1: m_buffer carries BOTH directions (EVENT_INPUT = NCP->host frame,
	// EVENT_OUTPUT = host->NCP chunk). It must hold the largest protocol frame
	// or such frames get rejected below *after* transport already queued them
	// into m_input_buf -> the bytes strand and the link desyncs.
	static_assert(BUFFER_SIZE >= protocol::MAX_FRAME_SIZE,
	              "app::BUFFER_SIZE must cover protocol::MAX_FRAME_SIZE (H1)");
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
        case EVENT_RESET: {
            // The NCP_RESET request handler posts EVENT_RESET from
            // process_status_arg BEFORE immediate_cmd_process::process gets
            // to call send_cmd_data — so the actual NCP_RESET response is
            // queued in m_queue as EVENT_INPUT BEHIND this EVENT_RESET.
            //
            // Drain remaining queue entries first so any pending response
            // (most importantly the OK reply to the very NCP_RESET request
            // that triggered this reset) goes out via usb_serial_jtag_write_bytes
            // before we reboot. Without the drain z2m's reset() promise
            // times out (no matching-tsn response) and z2m exits with
            // "Failed to start zigbee-herdsman".
            //
            // Then a short delay lets the USB-Serial-JTAG controller actually
            // shift the queued bytes out onto the wire before esp_restart()
            // yanks the rug — usb_serial_jtag_write_bytes returns once the
            // bytes are accepted into the driver's TX ring, not after they
            // are transmitted.
            ctx_t pending;
            while (xQueueReceive(m_queue, &pending, 0) == pdTRUE) {
                process_event(pending);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } break;
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

    // Boot-time framing-level ACK. Sent immediately at start-up so z2m's
    // open-port handshake doesn't time out waiting for ANY frame from us.
    // Carries no NVRAM-state-dependent fields — flags byte is_ack=1 with
    // packet_seq=0 and ack_seq=0 (the "wake up call" pattern from
    // andryblack/esp-coordinator#11).
    uint8_t raw_data[] = {0xDE, 0xAD, 0x05, 0x00, 0x06, 0x01, 0x8F};
    transport::send(raw_data, sizeof(raw_data));

    // Cold-boot panID race: start the ZBOSS dispatch task here, after the
    // transport polling task is up and the event loop is about to drain
    // m_queue. Triggers zboss_main_loop -> SKIP_STARTUP -> continue_zboss,
    // which sends the synthetic NCP_RESET response and kicks off
    // NETWORK_STEERING so the persisted NVRAM-stored network is actually
    // active by the time z2m's first GET_JOINED query arrives. Without this
    // the task only spins up lazily when the host issues NWK_FORMATION /
    // NWK_START_WITHOUT_FORMATION — but z2m doesn't issue those at startup
    // (it queries first), and on stale defaults it formNetwork()s and wipes
    // every paired device.  See andryblack/esp-coordinator#5/#19, z2m #26152.
    zb_ncp::start_zigbee_stack();

    // The synthetic NCP_RESET *response* (cmd=0x0002, tsn=0xFF, status=OK)
    // used to be sent here, but doing so before ZBOSS finished loading its
    // NVRAM made z2m proceed to needsToBeInitialised() too early:
    // GET_PAN_ID / GET_EXTENDED_PAN_ID / GET_ZIGBEE_CHANNEL returned the
    // uninitialised defaults (0xFFFF / 0xFF), z2m saw a "different" network
    // than its configured options, and called formNetwork() — which trashes
    // the entire device DB.  See andryblack/esp-coordinator#5 (z2m #26152)
    // for the original report and #19 for the regression confirming PR #6's
    // 'move zboss_start_no_autostart() into init_int()' alone wasn't
    // sufficient: the NVRAM load itself only completes inside zboss_main_loop
    // (on the dedicated ZBOSS FreeRTOS task), and we used to race against
    // that here.  The boot-ready frame is now sent from zb_ncp::continue_zboss,
    // which is scheduled from the ZB_ZDO_SIGNAL_SKIP_STARTUP handler — the
    // earliest signal at which the dataset is guaranteed loaded and
    // zb_get_pan_id() returns the persisted value.

 	// uint8_t outdata[2] = { 0,0 };
    // esp_ncp_header_t header = {{0,0,0,},NCP_RESET,0,0};
    // esp_ncp_resp_input(&header, outdata, sizeof(outdata)); 

	ctx_t ctx;
    while (true) {
        if (xQueueReceive(m_queue, &ctx, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (process_event(ctx) != ESP_OK) {
            // Per-event failure must not tear down the NCP link — a transient
            // stream-buffer or oversize-packet error on one ctx is independent
            // of the next. Pre-fix this loop would `break`, returning to
            // app_main which then ended the main task and left the host with
            // an open serial port that never answered again.
            ESP_LOGE(TAG, "Process event fail");
        }
    }
	return ESP_OK;
}

esp_err_t app::init() {
	ESP_LOGI(TAG,"init");
	auto res = nvs_flash_init();
	if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		// M2: a host RESTORE_NETWORK can leave nvs in a layout this IDF build
		// can't mount. Erase + re-init instead of returning the error up into
		// ESP_ERROR_CHECK(app::init()) in main.cpp, which would abort and boot-
		// loop the device with the host's serial port never answering again.
		ESP_LOGW(TAG, "nvs_flash_init: %s -- erasing and retrying", esp_err_to_name(res));
		nvs_flash_erase();
		res = nvs_flash_init();
	}
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