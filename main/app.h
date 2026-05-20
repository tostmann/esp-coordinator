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

	// Must be >= transport::BUF_SIZE (currently 1024) — undersizing causes silent
	// drops of bursts >BUFFER_SIZE in process_event (ESP_ERR_NO_MEM path).
	static constexpr size_t BUFFER_SIZE = 1024;

	QueueHandle_t m_queue; 
	uint8_t m_buffer[BUFFER_SIZE];

public:
	
	static esp_err_t send_event(const ctx_t& ctx){ return instance().send_event_int(ctx); }

	static esp_err_t init();
	static esp_err_t start();

	static void on_rx_data(const void* data, size_t size);
};