#pragma once
#include <cstdint>
#include <atomic>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "sdkconfig.h"

class transport {
private:
	static constexpr size_t BUF_SIZE = 1024;
	static constexpr size_t RINGBUF_SIZE = 1024*20;
	static constexpr size_t RINGBUF_TIMEOUT_MS = 50;
	static constexpr size_t TASK_STACK = 1024*4;
	static constexpr size_t TASK_PRIORITY = 18;

	// Which host interface NCP->host frames are routed to. Set by the RX
	// tasks (the interface that last delivered host bytes wins), read by
	// write_int on the app task. IFACE_NONE = no host has spoken since boot;
	// the boot-time frames are then offered to every interface with strictly
	// non-blocking writes. Running two talking hosts at once is unsupported
	// (single NCP link-layer state) — the routing exists so an idle,
	// unserviced interface can never stall or pollute the active one.
	enum iface_t : uint8_t { IFACE_NONE = 0, IFACE_USB = 1, IFACE_UART = 2 };

	transport();
	static transport& instance();
	esp_err_t init_int();
	esp_err_t start_int();
	StreamBufferHandle_t m_input_buf;                    /*!< The pointer to storage the data from NCP */
    StreamBufferHandle_t m_output_buf;                   /*!< The pointer to storage the data to NCP */
    SemaphoreHandle_t m_input_sem;        /*!< A semaphore handle for process the data from NCP */
    SemaphoreHandle_t m_output_sem;       /*!< Serialises the USB/UART RX tasks writing m_output_buf
                                               (FreeRTOS stream buffers are single-writer) */

	std::atomic<uint8_t> m_active{IFACE_NONE};

	uint8_t m_temp_buf[BUF_SIZE];

	esp_err_t write_int(const void* data,size_t size);
	void task_int();
	void rx_pump(const uint8_t* data, int readed);
	void rx_qualified_pump(iface_t iface, bool& pending_de, const uint8_t* data, int readed);

	static void task(void *pvParameter) {
		static_cast<transport*>(pvParameter)->task_int();
	}

	bool m_usb_pending_de = false;        /*!< USB RX signature-matcher state (chunk ended in 0xDE) */

#if CONFIG_NCP_UART_TRANSPORT
	bool m_uart_ok = false;               /*!< UART driver installed; gate for the UART RX task */
	bool m_uart_pending_de = false;       /*!< UART RX signature-matcher state */
	uint8_t m_uart_temp_buf[BUF_SIZE];

	esp_err_t uart_init_int();
	void uart_task_int();

	static void uart_task(void *pvParameter) {
		static_cast<transport*>(pvParameter)->uart_task_int();
	}
#endif

	esp_err_t send_int(const void* data,size_t size);

	esp_err_t process_input_int(void* buffer,size_t size);
public:
	static esp_err_t init() { return instance().init_int(); }
	static esp_err_t start() { return instance().start_int(); }

	static esp_err_t send(const void* data,size_t size) { return instance().send_int(data,size); }

	static esp_err_t process_input(void* buffer,size_t size) { return instance().process_input_int(buffer,size); }
	static size_t output_receive(void* buffer,size_t size);
};
