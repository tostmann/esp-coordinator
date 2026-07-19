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
	static constexpr size_t RINGBUF_SIZE = 1024*20;   // R4(b) 32 KB bump REVERTED — OOMed xStreamBufferCreate at transport::init (heap is tighter than assumed with WiFi+ZBOSS+mDNS up) -> app::init abort -> boot loop. 20 KB is the proven size; R4(a) SO_SNDTIMEO is the actual unlock. Revisit INDICATION-burst headroom only after measuring real free heap.
	static constexpr size_t RINGBUF_TIMEOUT_MS = 50;
	static constexpr size_t TASK_STACK = 1024*4;
	static constexpr size_t TCP_TASK_STACK = 1024*6;   // lwIP socket task needs more headroom
	static constexpr size_t TASK_PRIORITY = 18;

	// Which host interface NCP->host frames are routed to. Set by the RX
	// tasks (the interface that last delivered host bytes wins), read by
	// write_int on the app task. IFACE_NONE = no host has spoken since boot;
	// the boot-time frames are then offered to every interface with strictly
	// non-blocking writes. Running two talking hosts at once is unsupported
	// (single NCP link-layer state) — the routing exists so an idle,
	// unserviced interface can never stall or pollute the active one.
	// IFACE_TCP (wifi-coex Mode B) joins as a third interface: the TCP RX
	// loop qualifies its client through the same DE-AD signature gate, so a
	// port scanner that merely connects can neither steal the route nor
	// inject garbage into the shared frame stream.
	enum iface_t : uint8_t { IFACE_NONE = 0, IFACE_USB = 1, IFACE_UART = 2, IFACE_TCP = 3 };

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

	// wifi-coex normal Mode B reserves USB-Serial/JTAG for the Improv reconfig
	// window (disable_usb_ncp() before start()): the USB RX task is not started
	// and write_int() never touches the endpoint, so Improv owns it exclusively
	// (no two-reader/two-writer split with the NCP framer). Left true for the
	// plain USB build and for Mode-B safe mode (both serve NCP over USB).
	bool m_serve_usb_ncp = true;

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

	// wifi-coex Mode B: NCP frame stream additionally on a raw TCP server.
	// Enabled at runtime by use_tcp() before start(); the TCP RX loop runs as
	// a third RX task next to USB/UART, write_int() routes to the client
	// socket when IFACE_TCP is active (one whole DEAD frame per send,
	// TCP_NODELAY, SO_SNDTIMEO-bounded — R4).
	bool m_use_tcp = false;
	uint16_t m_tcp_port = 0;
	int m_listen_sock = -1;
	int m_tcp_sock = -1;            // current client fd, -1 when none connected
	SemaphoreHandle_t m_tcp_fd_sem; /*!< Guards m_tcp_sock lifecycle: write_int's
	                                     send/shutdown vs tcp_task's close/re-accept.
	                                     Without it a stale fd snapshot can hit the
	                                     NEXT client's reused lwIP slot (or the link
	                                     watchdog's probe socket). Leaf lock — never
	                                     taken while holding another transport sem. */
	bool m_tcp_pending_de = false;  /*!< TCP RX signature-matcher state */
	uint8_t m_tcp_temp_buf[BUF_SIZE];   // own buffer: USB task_int owns m_temp_buf concurrently
	void tcp_task_int();
	static void tcp_task(void *pvParameter) {
		static_cast<transport*>(pvParameter)->tcp_task_int();
	}

	esp_err_t send_int(const void* data,size_t size);

	esp_err_t process_input_int(void* buffer,size_t size);
public:
	static esp_err_t init() { return instance().init_int(); }
	static esp_err_t start() { return instance().start_int(); }

	// Additionally serve the NCP stream on a TCP server on `port` (wifi-coex
	// Mode B). Must be called before start(); default (uncalled) runs the
	// serial interfaces (USB-Serial/JTAG + UART) only.
	static void use_tcp(uint16_t port) { instance().m_use_tcp = true; instance().m_tcp_port = port; }

	// Reserve USB-Serial/JTAG for Improv reconfig (wifi-coex normal Mode B): the
	// USB RX task is not started and write_int() skips the USB endpoint, so the
	// Improv provisioner owns it without contending with the NCP framer. Must be
	// called before start(). NCP hosts then use TCP (+ UART) only.
	static void disable_usb_ncp() { instance().m_serve_usb_ncp = false; }

	// Send a FIN to a connected TCP client (shutdown only — the close stays
	// with tcp_task, whose recv unblocks on the shutdown). Called by the
	// reboot paths (NCP_RESET / RESTORE_NETWORK) right before esp_restart:
	// without it the device reboots, WiFi drops, and the host is left on a
	// silently dead connection with no FIN/RST — it black-holes instead of
	// reconnecting for the boot-ready frame.
	static void close_tcp_client();

	// True while TCP is the active NCP route (the last qualified host bytes
	// arrived over TCP). Lets command-layer session logic key its "the TCP
	// client vanished" cleanup to sessions TCP actually carries — a zombie
	// TCP client dying must not abort a restore streaming over UART
	// (merge-review finding, 2026-07-19).
	static bool tcp_route_active() { return instance().m_active.load() == IFACE_TCP; }

	static esp_err_t send(const void* data,size_t size) { return instance().send_int(data,size); }

	static esp_err_t process_input(void* buffer,size_t size) { return instance().process_input_int(buffer,size); }
	static size_t output_receive(void* buffer,size_t size);
};
