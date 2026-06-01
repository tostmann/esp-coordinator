#pragma once
#include <esp_err.h>

#include <freertos/FreeRTOS.h>
#include <sys/queue.h>

class app {
public:
	enum event_t : uint16_t {
	   EVENT_INPUT,                /*!< Input event from NCP to host */
	   EVENT_OUTPUT,               /*!< Output event from host to NCP */
	   EVENT_RESET,                /*!< Reset event from host to NCP */
	};
	struct ctx_t {
		event_t event;	/*!< The event between the host and NCP */
		uint16_t size;	/*!< Data size on the event */
	};
private:
	app();
	static app& instance();
	esp_err_t send_event_int(const ctx_t& ctx);
	esp_err_t init_int();
	esp_err_t start_int();

	esp_err_t process_event(const ctx_t& ctx);
	static constexpr size_t EVENT_QUEUE_LEN = 60;
	static constexpr size_t TIMEOUT_MS  = 10;

	// Sized to the LARGER of the two directions multiplexed through m_buffer in
	// process_event:
	//   - host->NCP read chunk = transport::BUF_SIZE       (1024)
	//   - NCP->host frame       = protocol::MAX_FRAME_SIZE  (2048; full 64-entry
	//                             Mgmt_Lqi / Mgmt_Bind responses)
	// Undersizing strands oversize NCP->host frames: transport commits them to
	// m_input_buf, then process_event rejects ctx.size>BUFFER_SIZE and the bytes
	// rot in the FIFO -> permanent link desync (H1). The static_assert in
	// app.cpp ties this to protocol's value so the two layers can't drift.
	static constexpr size_t BUFFER_SIZE = 2048;

	QueueHandle_t m_queue; 
	uint8_t m_buffer[BUFFER_SIZE];

public:
	
	static esp_err_t send_event(const ctx_t& ctx){ return instance().send_event_int(ctx); }

	static esp_err_t init();
	static esp_err_t start();

	static void on_rx_data(const void* data, size_t size);
};