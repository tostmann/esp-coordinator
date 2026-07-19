#include "app.h"
#include "boot_guard.h"
#include "transport.h"
#include "protocol.h"
#include "zb_ncp.h"
#include "netcfg.h"
#include "wifi_coex.h"
#include "improv_provisioning.h"
#include "web_info.h"
#include "sdkconfig.h"
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
        // MUST stay non-blocking (timeout 0): this path is reached from the
        // ZBOSS task via send_cmd_data -> protocol::send_data ->
        // transport::send, and ZBOSS invokes that from INSIDE its
        // zb_osif_disable_all_interrupts/enable critical window (MAC logic
        // iteration). A blocking xQueueSend there lets the scheduler switch
        // away inside the critical section and corrupts the per-CPU critical
        // nesting count -> "assert failed: vPortExitCritical port.c:618
        // (port_uxCriticalNesting[0] > 0)" panic on ncp_zb_task — reproduced
        // 3x on 2026-06-06 under interview/INDICATION bursts (full queue =
        // the only time the blocking branch engages). Callers that CAN
        // safely wait (the transport RX tasks) implement their own bounded
        // retry around this call instead (transport::rx_pump).
        ret = xQueueSend(m_queue, &ctx, 0);
        if (ret != pdTRUE) {
            ESP_LOGE(TAG, "event queue full, event %d (size %u) dropped",
                     ctx.event, (unsigned)ctx.size);
        }
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
        case EVENT_TCP_DISCONNECT:
            // Session-scoped resource cleanup, executed on the app task (the
            // owner of all command state). Currently: drop the
            // GET_NETWORK_BACKUP RAM snapshot a vanished client left behind.
            zb_ncp::release_backup_snapshot();
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
    // wifi-coex variant: a hard boot-time MODE GATE on "are STA creds present?".
    // Improv-Serial provisioning and the binary NCP DEAD-frame stream never
    // share the USB-Serial/JTAG wire simultaneously, so no per-byte demux is
    // needed and a coincidental 'IMPROV' substring inside an NCP payload can
    // never be mis-parsed as a provisioning frame.
    if (!netcfg_has_sta_creds()) {
        // MODE A — provisioning. The USB-Serial/JTAG driver is already installed
        // (transport::init in init_int); we do NOT start the transport poll task,
        // do NOT send the boot ACK, and do NOT start the Zigbee stack — so no
        // unsolicited DEAD frame ever hits the line. Improv runs on its own task
        // reading/writing the USB-JTAG endpoint directly; on success it persists
        // creds and reboots into Mode B.
        ESP_LOGI(TAG, "Mode A: no STA creds -> Improv-Serial provisioning");
        // Mode A never starts ZBOSS, so continue_zboss()/mark_zboss_alive() are
        // never reached and the boot guard's 30 s assume-fail deadline would
        // otherwise reboot the provisioning session (and, after 3 reboots, latch
        // the stick into safe mode). Stand the guard down: provisioning is a
        // legitimate, unbounded non-ZBOSS state.
        boot_guard::cancel();
        improv_provisioning_start();
    } else {
        // MODE B — operational. Bring up WiFi STA (WIFI_PS_NONE first, C6 rule),
        // switch the host link to the TCP NCP server, then start the Zigbee
        // stack. The boot framing ACK is (re)emitted on each TCP accept by the
        // transport (a TCP client connects later than boot, unlike the always-
        // open USB port). The NCP_RESET boot-ready frame + esp_coex_wifi_i154_enable()
        // + esp_wifi_connect() all run from zb_ncp::continue_zboss
        // (ZB_ZDO_SIGNAL_SKIP_STARTUP), mirroring the esp_zigbee_gateway flow
        // that was HW-confirmed on a C6.
        ESP_LOGI(TAG, "Mode B: STA creds present -> WiFi coex + NCP-over-TCP :%d", CONFIG_NCP_TCP_PORT);
        const bool safe = boot_guard::safe_mode();
        auto werr = wifi_coex_init();
        if (werr != ESP_OK)
            return werr;
        transport::use_tcp(CONFIG_NCP_TCP_PORT);
        if (!safe) {
            // Normal Mode B: dedicate USB-Serial/JTAG to the 120 s Improv
            // reconfig window (started below) so a user can re-enter WiFi creds
            // even while WiFi is up. The NCP host links here are TCP (primary) +
            // UART1; USB is NOT an NCP interface, so Improv and the NCP framer
            // never contend for the endpoint. Safe mode keeps USB serving the
            // NCP recovery whitelist instead (see below) and skips the window.
            transport::disable_usb_ncp();
        }
        auto res = transport::start();
        if (res != ESP_OK)
            return res;

        // Boot framing ACK for the SERIAL interfaces (UART NCP hosts stay
        // available in Mode B next to TCP; USB serves NCP only in safe mode —
        // normal Mode B dedicates it to Improv via disable_usb_ncp above,
        // andryblack#11): at
        // this point m_active is IFACE_NONE, so the frame is offered to every
        // serial interface non-blockingly. TCP clients don't need it here —
        // they connect later and get it re-emitted on each accept
        // (transport::tcp_task_int).
        uint8_t raw_data[] = {0xDE, 0xAD, 0x05, 0x00, 0x06, 0x01, 0x8F};
        transport::send(raw_data, sizeof(raw_data));

        if (safe) {
            // WEDGE-1 safe mode: repeated early-boot failures — keep the host
            // link up WITHOUT the ZBOSS stack (dispatch restricted in
            // zb_ncp::on_rx_data) so the stick stays reachable and recoverable
            // over TCP, not just USB/UART. continue_zboss() never runs in safe
            // mode, so the WiFi STA it normally brings up
            // (wifi_coex_start_connect at SKIP_STARTUP) would never associate —
            // the TCP server task would block forever on wifi_coex_is_up() and
            // never bind. Start the STA here so the recovery host can reach us
            // over TCP to send NCP_RESET (options=2 = remote factory-erase of
            // the NVRAM a wedged stick chokes on). No radio-coex enable: there
            // is no 802.15.4 stack to coexist with this boot.
            // Also send the boot-ready frame here (continue_zboss won't); hosts
            // then fail visibly on GENERIC_BLOCKED getters instead of a silent
            // open-port timeout. NO early return — the event pump at the end of
            // this function is what drains m_queue (incl. these very frames and
            // every command response); returning here would leave the link mute.
            ESP_LOGE(TAG, "boot guard SAFE MODE: ZBOSS not started");
            wifi_coex_start_connect();
            zb_ncp::send_boot_ready_frame();
        } else {
            // Cold-boot panID race: start the ZBOSS dispatch task here, after
            // the transport polling task is up and the event loop is about to
            // drain m_queue. Triggers zboss_main_loop -> SKIP_STARTUP ->
            // continue_zboss, which resumes the persisted NVRAM network
            // (ZB_BDB_INITIALIZATION -> DEVICE_REBOOT, per BOOT-1) so it is
            // active by the time z2m's first GET_JOINED query arrives. Without
            // this the task only spins up lazily when the host issues
            // NWK_FORMATION / NWK_START_WITHOUT_FORMATION — but z2m doesn't
            // issue those at startup (it queries first), and on stale defaults
            // it formNetwork()s and wipes every paired device.
            // See andryblack/esp-coordinator#5/#19, z2m #26152.
            zb_ncp::start_zigbee_stack();

            // 120 s post-boot Improv-Serial reconfig window on USB-Serial/JTAG
            // (Dirk: reconfigure WiFi even when WiFi is up). USB-JTAG was
            // reserved for Improv above (disable_usb_ncp). CaptureBackend
            // persists the new creds + reboots; wifi_coex applies them next
            // boot. The task frees itself once the window closes.
            improv_provisioning_start_reconfig();

            // On-device info page: after WiFi is up, http://<ip>/ (the Improv
            // "Visit Device" URL) serves the Z2M-over-TCP setup instructions
            // with this device's own address filled in. Waits for got-IP
            // internally; normal Mode B only (not safe mode).
            web_info_start();
        }
    }

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
        // Bounded wait so the request watchdog runs even on an idle link; on a
        // busy link it runs after each event (self-rate-limited internally).
        if (xQueueReceive(m_queue, &ctx, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (process_event(ctx) != ESP_OK) {
                // Per-event failure must not tear down the NCP link — a transient
                // stream-buffer or oversize-packet error on one ctx is independent
                // of the next. Pre-fix this loop would `break`, returning to
                // app_main which then ended the main task and left the host with
                // an open serial port that never answered again.
                ESP_LOGE(TAG, "Process event fail");
            }
        }
        zb_ncp::request_watchdog_tick();
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