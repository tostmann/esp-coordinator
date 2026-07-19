#include "transport.h"
#include "app.h"
#include "protocol.h"
#include "utils.h"

#include <esp_log.h>
#include <esp_timer.h>
#include "sdkconfig.h"

#include <driver/usb_serial_jtag.h>
#if CONFIG_NCP_UART_TRANSPORT
#include <driver/uart.h>
#include <driver/gpio.h>
#endif
#include <cstring>

#include "wifi_coex.h"
#include <lwip/sockets.h>

static const char* TAG = "TRNPT";

// R4 overall per-frame deadline for the active-TCP send loop. SO_SNDTIMEO
// (8 s, set on accept) bounds each ::send, but lwIP restarts that clock per
// call and returns partial counts — a trickling peer could otherwise park
// the app task (and hold m_tcp_fd_sem) far beyond the intended bound.
static constexpr uint32_t TCP_FRAME_DEADLINE_MS = 12000;

#if CONFIG_NCP_UART_TRANSPORT
// UART1 fixed: UART0 is the (silent) primary console and carries the ROM
// bootloader's reset banner on GPIO16/17 — routing the NCP stream there would
// inject garbage into the host parser on every reset. UART1 has no IOMUX
// binding on the C6; the Kconfig pins go through the GPIO matrix.
static constexpr uart_port_t NCP_UART_PORT = UART_NUM_1;
// uart_write_bytes enqueues an event item plus data item(s) into a NOSPLIT
// ring buffer (8-byte header per item) and busy-spins when the ring is full.
// The free-size gate in write_int keeps a margin for those overheads so the
// call is only made when the whole frame fits without blocking.
static constexpr size_t UART_TX_ITEM_MARGIN = 32;
#endif

transport::transport() {}

transport& transport::instance() {
	static transport s_transport;
	return s_transport;
}

esp_err_t transport::write_int(const void *buffer, size_t size)
{
    // Tri-interface TX routing (see iface_t in transport.h). Frames go only
    // to the interface the host last spoke on; in IFACE_NONE (boot frames,
    // before any host byte) they are offered to every interface with
    // strictly non-blocking writes.
    //
    // Why non-blocking on the non-active path is load-bearing:
    // usb_serial_jtag_write_bytes copies into the driver's TX ring, which
    // only drains while a USB host performs IN transactions. With no host
    // attached the ring fills once and every further timed write blocks for
    // the full timeout — stalling the app task and throttling the *other*
    // (active) interface to ~20 frames/s. The driver's xRingbufferSend is
    // all-or-nothing, so a 0-tick write either queues the whole frame or
    // nothing — no torn frames. (Verified against IDF 5.5.3
    // esp_driver_usb_serial_jtag/src/usb_serial_jtag.c.)
    const uint8_t active = m_active.load(std::memory_order_relaxed);
    bool delivered = false;

    // TCP client (wifi-coex Mode B). Only the ACTIVE-TCP path writes the
    // socket — IFACE_NONE deliberately does NOT offer frames to a connected
    // client: a fresh client gets its boot frames directly from tcp_task at
    // accept, and anything else sent pre-qualification would carry stale
    // sequence state (the route only flips, and the link state only resets,
    // once the client's first DE-AD frame arrives in rx_qualified_pump).
    //
    // R4 semantics on the active path: SO_SNDTIMEO (set on accept) bounds
    // each ::send and an overall per-frame deadline bounds the whole loop
    // (a trickling peer restarts the SNDTIMEO clock with every partial
    // accept), so the single app task can NOT park indefinitely on a
    // coex-starved WiFi link — it must stay free to drain m_input_buf,
    // serve the next host command AND deliver Zigbee INDICATION frames
    // (the data path the owner prioritises). On a stall: if nothing of this
    // frame reached the wire (off==0, the common "TCP send buffer full under
    // starvation" case) just drop the frame — the host re-requests and the
    // stream stays frame-aligned. If a partial frame already went out the
    // host's DEAD stream is desynced, so force a clean resync by tearing the
    // connection (tcp_task's recv breaks -> close -> re-accept; the host
    // re-syncs on the per-accept boot frames).
    //
    // The whole section holds m_tcp_fd_sem so tcp_task cannot close (and a
    // re-accept cannot reuse) the fd mid-send — the teardown waits here at
    // most the per-frame deadline.
    if (m_use_tcp && active == IFACE_TCP) {
        utils::sem_lock l(m_tcp_fd_sem);
        int fd = m_tcp_sock;
        if (fd >= 0) {
            const uint8_t *p = static_cast<const uint8_t *>(buffer);
            const TickType_t deadline =
                xTaskGetTickCount() + pdMS_TO_TICKS(TCP_FRAME_DEADLINE_MS);
            size_t off = 0;
            bool fail = false;
            while (off < size) {
                int n = ::send(fd, p + off, size - off, 0);
                if (n <= 0) {
                    fail = true;     // SNDTIMEO expired with zero progress
                    break;
                }
                off += static_cast<size_t>(n);
                if (off < size && xTaskGetTickCount() >= deadline) {
                    fail = true;     // trickling peer: partials keep restarting
                    break;           // the SNDTIMEO clock, the deadline doesn't
                }
            }
            if (fail) {
                if (off > 0) {
                    ::shutdown(fd, SHUT_RDWR);
                }
                ESP_LOGE(TAG, "tcp send stalled at %u/%u bytes -- %s",
                         (unsigned)off, (unsigned)size,
                         off ? "tearing connection" : "frame dropped");
            } else {
                delivered = true;
            }
        }
        // fd < 0: client vanished mid-session — the frame is dropped; the
        // host re-requests after reconnecting.
    }

    if (m_serve_usb_ncp && (active == IFACE_USB || active == IFACE_NONE)) {
        const TickType_t ticks = (active == IFACE_USB) ? pdMS_TO_TICKS(RINGBUF_TIMEOUT_MS) : 0;
        if (usb_serial_jtag_write_bytes(buffer, size, ticks) == (int)size) {
            delivered = true;
        } else if (active == IFACE_USB) {
            ESP_LOGE(TAG, "usb write failed (%u bytes)", (unsigned)size);
        }
    }

#if CONFIG_NCP_UART_TRANSPORT
    if (m_uart_ok && (active == IFACE_UART || active == IFACE_NONE)) {
        // uart_write_bytes has NO timeout parameter — it enqueues with
        // portMAX_DELAY / busy-spins when the TX ring is full. Gate it on the
        // ring having room for the whole frame (+ item-header margin) so the
        // call below can never block. The ring drains at baud rate regardless
        // of a listener, so on the active interface a short bounded wait
        // mirrors the USB-path semantics; boot frames don't wait at all.
        size_t free_size = 0;
        uart_get_tx_buffer_free_size(NCP_UART_PORT, &free_size);
        if (active == IFACE_UART) {
            int count = RINGBUF_TIMEOUT_MS / 10;
            while (free_size < size + UART_TX_ITEM_MARGIN && count-- > 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                uart_get_tx_buffer_free_size(NCP_UART_PORT, &free_size);
            }
        }
        if (free_size >= size + UART_TX_ITEM_MARGIN) {
            if (uart_write_bytes(NCP_UART_PORT, buffer, size) == (int)size) {
                delivered = true;
            }
        } else if (active == IFACE_UART) {
            ESP_LOGE(TAG, "uart tx ring full, dropping frame (%u bytes)", (unsigned)size);
        }
    }
#endif

    return delivered ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void transport::rx_pump(const uint8_t* data, int readed) {
    app::ctx_t ncp_event = {
        .event = app::EVENT_OUTPUT,
        .size = 0
    };

    // TRNPT-2: xStreamBufferSend can accept FEWER than `readed` bytes when
    // m_output_buf is near-full. Posting EVENT_OUTPUT with the full read
    // size then over-claims and output_receive desyncs the byte stream.
    // Post only what was actually committed, and push the remainder under a
    // bounded wait so transient backpressure doesn't silently drop host
    // bytes. Splitting one read into several EVENT_OUTPUT posts is safe —
    // the protocol layer reassembles frames across on_rx calls.
    //
    // Stream buffers are single-writer; with the UART transport enabled two
    // RX tasks feed m_output_buf, so the send+event pair is serialised under
    // m_output_sem (also keeps the EVENT_OUTPUT ordering consistent with the
    // byte order in the buffer).
    utils::sem_lock l(m_output_sem);
    int off = 0;
    while (off < readed) {
        size_t sent = xStreamBufferSend(m_output_buf, data + off,
                                        readed - off,
                                        pdMS_TO_TICKS(RINGBUF_TIMEOUT_MS));
        if (sent == 0) {
            ESP_LOGE(TAG, "output_buf full, dropping %d bytes", readed - off);
            break;
        }
        ncp_event.size = sent;
        // Bounded retry on a momentarily-full event queue: the bytes are
        // already committed to m_output_buf, so a dropped event would orphan
        // them and shift the byte/event pairing. Retrying HERE is safe —
        // rx_pump only ever runs on our own RX tasks (USB/UART/TCP), never
        // on the ZBOSS task (app::send_event itself must stay non-blocking
        // for that caller — see the critical-section panic note there).
        int tries = RINGBUF_TIMEOUT_MS / 10;
        while (app::send_event(ncp_event) != ESP_OK && tries-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        off += sent;
    }
}

void transport::rx_qualified_pump(iface_t iface, bool& pending_de, const uint8_t* data, int readed) {
    // Hardened routing flip (pre-release hardening 2026-06-05): an interface
    // only becomes the active TX target once a ZBOSS-NCP frame signature
    // (DE AD) shows up in its RX stream. Without this, ANY received byte
    // flipped the route — and host-side port probers (canonical case:
    // ModemManager sending AT commands a few seconds after every USB
    // re-enumeration, which herdsman triggers via NCP_RESET at each adapter
    // start) could steal the route from a live UART session and black-hole
    // the in-flight response. Bytes from a non-active interface are
    // discarded up to the first DE AD, so prober garbage also can't be
    // injected into the shared frame stream. The matcher state survives
    // chunk boundaries via `pending_de` (chunk ended in 0xDE).
    if (m_active.load(std::memory_order_relaxed) == iface) {
        pending_de = (data[readed - 1] == 0xDE);   // keep matcher state fresh
        rx_pump(data, readed);
        return;
    }

    int sig = -1;                                  // index of 0xDE, or -1 for "spans chunks"
    bool spans = (pending_de && data[0] == 0xAD);
    if (!spans) {
        for (int i = 0; i + 1 < readed; ++i) {
            if (data[i] == 0xDE && data[i + 1] == 0xAD) {
                sig = i;
                break;
            }
        }
        if (sig < 0) {
            pending_de = (data[readed - 1] == 0xDE);
            return;                                // no signature yet — drop garbage
        }
    }

    if (iface == IFACE_TCP) {
        // A fresh TCP client qualifies (first DE-AD frame seen): re-baseline
        // the protocol layer to cold-boot state so the new host — whose own
        // sequence restarts at 0 — sees our first dynamic frame at seq 1,
        // exactly like a fresh boot (the herdsman reconnect fix, B). Doing
        // this at QUALIFICATION instead of at accept() means an unqualified
        // connect (port scanner, network monitor) can no longer re-baseline
        // an active serial session's link state — the DE-AD gate's guarantee
        // holds for the link state too, not just the route.
        protocol::reset_link_state();
    }
    m_active.store(iface, std::memory_order_relaxed);
    pending_de = false;
    if (spans) {
        const uint8_t de = 0xDE;                   // re-inject the 0xDE swallowed with the previous chunk
        rx_pump(&de, 1);
        rx_pump(data, readed);
    } else {
        rx_pump(data + sig, readed - sig);
    }
}

void transport::task_int() {
    while (true) {
        int readed = usb_serial_jtag_read_bytes(m_temp_buf, BUF_SIZE, pdMS_TO_TICKS(10));
        if (readed > 0) {
            rx_qualified_pump(IFACE_USB, m_usb_pending_de, m_temp_buf, readed);
        }
    }
}

#if CONFIG_NCP_UART_TRANSPORT
void transport::uart_task_int() {
    while (true) {
        int readed = uart_read_bytes(NCP_UART_PORT, m_uart_temp_buf, BUF_SIZE, pdMS_TO_TICKS(10));
        if (readed > 0) {
            rx_qualified_pump(IFACE_UART, m_uart_pending_de, m_uart_temp_buf, readed);
        }
    }
}
#endif

size_t transport::output_receive(void* buffer, size_t size) {
	return xStreamBufferReceive(instance().m_output_buf, buffer, size, pdMS_TO_TICKS(RINGBUF_TIMEOUT_MS));
}

esp_err_t transport::process_input_int(void* buffer, size_t size) {
	// xStreamBufferReceive can return less than `size` when the stream buffer's
	// trigger level fires earlier (created at level 8). The previous code then
	// bailed with ESP_FAIL, leaving the partial bytes to bleed into the next
	// EVENT_INPUT's batch and desynchronising the firmware->host frame stream.
	// Now we top-up under a single overall deadline so all `size` bytes either
	// arrive together or we fail cleanly with nothing partially written.
	size_t got = 0;
	const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RINGBUF_TIMEOUT_MS);
	while (got < size) {
		TickType_t now = xTaskGetTickCount();
		if (now >= deadline) break;
		auto chunk = xStreamBufferReceive(m_input_buf,
		                                  static_cast<uint8_t*>(buffer) + got,
		                                  size - got,
		                                  deadline - now);
		if (chunk == 0) break;
		got += chunk;
	}
	if (got != size) {
		ESP_LOGE(TAG, "Input buffer receive error: got %u expect %u!",
		         unsigned(got), unsigned(size));
		return ESP_FAIL;
	}
	return write_int(buffer, size);
}

esp_err_t transport::send_int(const void* data, size_t size) {
	if (data == NULL) {
        return ESP_FAIL;
    }

    app::ctx_t ncp_event = {
        .event = app::EVENT_INPUT,
        .size = 0
    };

    size_t ret_size = 0;

    {
        // H2: the capacity wait and the StreamBufferSend must be atomic w.r.t.
        // each other. app-task ACKs (send_ack/send_nack via on_rx_packet) race
        // ZBOSS-task send_data; with the space check outside the lock both
        // writers pass the gate and the second short-writes a partial CRC-
        // prefixed frame, desyncing the host stream. Holding m_input_sem across
        // the wait serialises the two writers. The wait stays bounded by
        // RINGBUF_TIMEOUT_MS, and the FIFO drainer is the app task (not parked
        // on this mutex), so no new deadlock is introduced.
        utils::sem_lock l(m_input_sem);

        int count = RINGBUF_TIMEOUT_MS / 10;
        while (xStreamBufferSpacesAvailable(m_input_buf) < size) {
            if (count == 0) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            count--;
        }
        if (count == 0) {
            ESP_LOGE(TAG, "input_buf not enough");
            return ESP_FAIL;
        }

        ret_size = xStreamBufferSend(m_input_buf, data, size, 0);
    }

    if (ret_size != size) {
        ESP_LOGE(TAG, "input_buf send error: size %d expect %d", ret_size, size);
        return ESP_FAIL;
    }
    ncp_event.size = size;
    return app::send_event(ncp_event);
}

esp_err_t transport::init_int() {
	ESP_LOGI(TAG,"init");

    m_input_buf = xStreamBufferCreate(RINGBUF_SIZE, 8);
    if (!m_input_buf) {
        ESP_LOGE(TAG, "Input buffer create error");
        return ESP_ERR_NO_MEM;
    }

    m_output_buf = xStreamBufferCreate(RINGBUF_SIZE, 8);
    if (!m_output_buf) {
        ESP_LOGE(TAG, "Out buffer create error");
        return ESP_ERR_NO_MEM;
    }

    m_input_sem = xSemaphoreCreateMutex();
    if (!m_input_sem) {
        ESP_LOGE(TAG, "Input semaphore create error");
        return ESP_ERR_NO_MEM;
    }

    m_output_sem = xSemaphoreCreateMutex();
    if (!m_output_sem) {
        ESP_LOGE(TAG, "Output semaphore create error");
        return ESP_ERR_NO_MEM;
    }

    m_tcp_fd_sem = xSemaphoreCreateMutex();
    if (!m_tcp_fd_sem) {
        ESP_LOGE(TAG, "TCP fd semaphore create error");
        return ESP_ERR_NO_MEM;
    }

    usb_serial_jtag_driver_config_t usb_serial_jtag_config;
    usb_serial_jtag_config.rx_buffer_size = BUF_SIZE * 2;
    usb_serial_jtag_config.tx_buffer_size = BUF_SIZE * 2;

    esp_err_t err = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB-JTAG driver already installed. Reusing interface.");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB-JTAG driver: %d", err);
        return err;
    }

#if CONFIG_NCP_UART_TRANSPORT
    // A failed UART bring-up degrades to USB-only instead of failing the
    // whole transport — the primary link must survive a bad pin config.
    if (uart_init_int() == ESP_OK) {
        m_uart_ok = true;
    } else {
        ESP_LOGE(TAG, "UART transport init failed -- continuing USB-only");
    }
#endif

    return ESP_OK;
}

#if CONFIG_NCP_UART_TRANSPORT
esp_err_t transport::uart_init_int() {
    uart_config_t cfg = {};
    cfg.baud_rate = CONFIG_NCP_UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    // TX ring gets extra slack: write_int drops frames when the ring can't
    // take a whole frame, and the ring only drains at baud rate.
    esp_err_t err = uart_driver_install(NCP_UART_PORT, BUF_SIZE * 2, BUF_SIZE * 4, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %d", err);
        return err;
    }
    err = uart_param_config(NCP_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %d", err);
        return err;
    }
    err = uart_set_pin(NCP_UART_PORT, CONFIG_NCP_UART_TX_GPIO, CONFIG_NCP_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %d", err);
        return err;
    }
    // uart_set_pin enables a pull-up only on CTS, NOT on RX (IDF 5.5.3
    // esp_driver_uart/src/uart.c). An unwired RX must idle high: noise here
    // would not only feed garbage into the shared NCP stream but also flip
    // m_active away from a live USB session.
    gpio_pullup_en((gpio_num_t)CONFIG_NCP_UART_RX_GPIO);

    ESP_LOGI(TAG, "UART transport on UART%d TX=%d RX=%d %d baud",
             (int)NCP_UART_PORT, CONFIG_NCP_UART_TX_GPIO, CONFIG_NCP_UART_RX_GPIO,
             CONFIG_NCP_UART_BAUD);
    return ESP_OK;
}
#endif

esp_err_t transport::start_int() {
	if (!m_input_buf) {
		ESP_LOGE(TAG,"need init");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG,"start%s", m_use_tcp ? " (+tcp)" : "");
	// Task-create failures DEGRADE instead of failing the whole transport:
	// returning ESP_FAIL here lands in ESP_ERROR_CHECK(app::start()) ->
	// abort() -> deterministic boot loop with no recovery path. Boot-time
	// heap demand grew with the tri-interface merge on the documented-tight
	// Mode-B heap, so each interface is brought up best-effort and the boot
	// only fails if NO host interface could be started at all (mirrors the
	// uart_init_int "primary link must survive" degrade).
	int started = 0;
	if (m_serve_usb_ncp) {
		if (xTaskCreate(&task, "transport", TASK_STACK, this, TASK_PRIORITY, NULL) == pdTRUE) {
			++started;
		} else {
			ESP_LOGE(TAG, "USB RX task create failed -- USB interface unavailable");
		}
	} else {
		// wifi-coex normal Mode B: USB-Serial/JTAG is reserved for the Improv
		// reconfig window (app::start_int). Not starting the USB RX task gives
		// Improv exclusive ownership of the endpoint. NCP hosts use TCP + UART.
		ESP_LOGI(TAG, "USB NCP interface off (reserved for Improv reconfig)");
	}
#if CONFIG_NCP_UART_TRANSPORT
	if (m_uart_ok) {
		if (xTaskCreate(&uart_task, "transport_uart", TASK_STACK, this, TASK_PRIORITY, NULL) == pdTRUE) {
			++started;
		} else {
			m_uart_ok = false;
			ESP_LOGE(TAG, "UART RX task create failed -- continuing without UART");
		}
	}
#endif
	if (m_use_tcp) {
		if (xTaskCreate(&tcp_task, "ncp_tcp", TCP_TASK_STACK, this, TASK_PRIORITY, NULL) == pdTRUE) {
			++started;
		} else {
			m_use_tcp = false;
			ESP_LOGE(TAG, "TCP task create failed -- continuing on serial interfaces only");
		}
	}
	if (started == 0) {
		ESP_LOGE(TAG, "no host interface could be started");
		return ESP_FAIL;
	}
	return ESP_OK;
}

// Feature 3 helper: scan a freshly-arrived chunk from a PENDING (not-yet-active)
// TCP client for the start of a ZBOSS-NCP frame (DE AD), so the client only
// PREEMPTS the active one once it proves it is a real host — a bare connect
// (port scan / monitor) never qualifies. `pending_de` carries the matcher across
// chunk boundaries (chunk ended in 0xDE). Returns: the index of the 0xDE if the
// DE AD is wholly inside this chunk; -1 if it completed via the carry (this chunk
// starts with 0xAD, the 0xDE was in the previous chunk); -2 if not seen yet.
static int scan_dead(const uint8_t* buf, int len, bool& pending_de) {
    if (pending_de && len > 0 && buf[0] == 0xAD) { pending_de = false; return -1; }
    for (int i = 0; i + 1 < len; ++i) {
        if (buf[i] == 0xDE && buf[i + 1] == 0xAD) { pending_de = false; return i; }
    }
    pending_de = (len > 0 && buf[len - 1] == 0xDE);
    return -2;
}

// wifi-coex Mode B host link: a single-client raw TCP server carrying the same
// DEAD-framed NCP stream the serial backends carry. Host-bound writes go
// through write_int (IFACE_TCP routing); host->NCP bytes are fed through the
// IDENTICAL rx_qualified_pump path the USB/UART tasks use, so the client is
// signature-gated and protocol/app reassemble frames the same way. Runs as a
// third RX task next to task()/uart_task() (own m_tcp_temp_buf; rx_pump
// serialises m_output_buf under m_output_sem).
void transport::tcp_task_int() {
	// bind() can only succeed once the STA has an IP.
	while (!wifi_coex_is_up()) {
		vTaskDelay(pdMS_TO_TICKS(200));
	}

	m_listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_listen_sock < 0) {
		ESP_LOGE(TAG, "socket() failed");
		vTaskDelete(NULL);
		return;
	}
	int opt = 1;
	setsockopt(m_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(m_tcp_port);
	if (::bind(m_listen_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
	    ::listen(m_listen_sock, 1) != 0) {
		ESP_LOGE(TAG, "bind/listen :%u failed (errno %d)", m_tcp_port, errno);
		::close(m_listen_sock);
		m_listen_sock = -1;
		vTaskDelete(NULL);
		return;
	}
	ESP_LOGI(TAG, "NCP-over-TCP listening on :%u", m_tcp_port);

	// Boot framing ACK — the open-port handshake z2m expects within ~1 s of
	// opening the port (andryblack/esp-coordinator#11). Over USB the port is
	// always open so app::start_int sends it once; over TCP a client connects
	// later (and may reconnect), so it is (re)emitted on every fresh accept.
	static const uint8_t boot_ack[] = {0xDE, 0xAD, 0x05, 0x00, 0x06, 0x01, 0x8F};

	while (true) {
		struct sockaddr_in cli = {};
		socklen_t clilen = sizeof(cli);
		int cs = ::accept(m_listen_sock, reinterpret_cast<struct sockaddr*>(&cli), &clilen);
		if (cs < 0) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}
		int one = 1;
		setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		setsockopt(cs, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
		// Detect a vanished host fast. lwIP defaults TCP_KEEPIDLE to 7200 s, so a
		// z2m that dies without FIN/RST (crash, OOM-kill, cable pull) would otherwise
		// pin this single-client server in a blocking recv() for up to 2 h, locking
		// out every new connection (the empirically-found ghost-connection gap).
		// Probe after 15 s idle (matching herdsman uart.js setKeepAlive(true,15000)),
		// then every 3 s, drop after 3 misses (~24 s) so recv() returns and the
		// accept loop frees the slot.
		int keep_idle = 15, keep_intvl = 3, keep_cnt = 3;
		setsockopt(cs, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
		setsockopt(cs, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
		setsockopt(cs, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));
		// R4: bound host-bound sends. Under coex the single radio is time-shared
		// with the always-RX Zigbee coordinator, so a WiFi TX window can be starved
		// for seconds; without this, a blocking ::send in write_int parks the app
		// task (the sole drainer of m_input_buf and server of every host command +
		// Zigbee INDICATION) until the stall clears — observed as the host seeing
		// the link go silent for 30 s+ and aborting. Cap the send so the task
		// recovers and stays responsive; write_int drops/​resyncs on expiry.
		struct timeval snd_to = {};
		snd_to.tv_sec = 8;
		setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

		// Per-accept boot frames go DIRECTLY to this socket — write_int routes
		// by m_active, which at this moment may still point elsewhere (the
		// route only flips, and the link state only resets, once the client's
		// first DE-AD frame arrives in rx_qualified_pump). SO_SNDTIMEO is
		// already set, so these sends are bounded. The socket is published
		// AFTER them, so no app-task write can interleave with the boot
		// sequence.
		::send(cs, boot_ack, sizeof(boot_ack), 0);

		// Also re-emit the NCP_RESET boot-ready frame (cmd=0x0002, tsn=0xFF, OK)
		// that z2m's open/reset handshake waits for. Over USB zb_ncp::continue_zboss
		// sends it once at boot; over TCP a client connects later, so re-send per
		// accept. KEEP IN SYNC with boot_ready_frame in zb_ncp.cpp::continue_zboss.
		// Timing-safe in Mode B: the TCP task only accepts after IP_EVENT_STA_GOT_IP,
		// which is after continue_zboss has loaded NVRAM (network-ready semantics hold).
		static const uint8_t ncp_reset_ready[] = {
			0xDE, 0xAD, 0x0E, 0x00, 0x06, 0xC0, 0x5D, 0x50, 0xD4,
			0x00, 0x01, 0x02, 0x00, 0xFF, 0x00, 0x00
		};
		::send(cs, ncp_reset_ready, sizeof(ncp_reset_ready), 0);

		m_tcp_pending_de = false;
		{
			utils::sem_lock l(m_tcp_fd_sem);
			m_tcp_sock = cs;
		}
		ESP_LOGI(TAG, "NCP client connected");

		int     nb            = -1;     // pending (unqualified) secondary client
		bool    nb_pending_de = false;  // its DE-AD signature-matcher carry
		int64_t nb_since_us   = 0;      // accept time (unqualified-drop timeout)

		// Feature 3: full teardown of the ACTIVE client `cs` — close under the
		// fd lock, demote the TX route, free the session's 40 KB backup snapshot.
		// Callable mid-session so a preemption can swap clients without breaking
		// the accept loop (the loop-exit teardown below handles the final one).
		auto teardown_active = [&]() {
			{
				utils::sem_lock l(m_tcp_fd_sem);
				m_tcp_sock = -1;
				::close(cs);
			}
			ESP_LOGW(TAG, "NCP client disconnected");
			uint8_t expected = IFACE_TCP;
			m_active.compare_exchange_strong(expected, IFACE_NONE,
			                                 std::memory_order_relaxed,
			                                 std::memory_order_relaxed);
			app::ctx_t ev = { .event = app::EVENT_TCP_DISCONNECT, .size = 0 };
			app::send_event(ev);
		};

		// Qualification-gated single-client server with new-client preemption:
		// a SECOND connection is accepted into a PENDING slot and given the boot
		// frames, but only PREEMPTS the active client once it sends a valid DE-AD
		// frame (scan_dead). A bare TCP connect (port scan / monitor) thus can
		// never kill a live z2m session, while a genuinely reconnecting host wins
		// immediately instead of waiting out the ~24 s keepalive on a half-open
		// zombie that holds the single slot.
		while (true) {
			fd_set rfds;
			FD_ZERO(&rfds);
			FD_SET(cs, &rfds);
			int maxfd = cs;
			if (nb >= 0) {
				FD_SET(nb, &rfds);
				if (nb > maxfd) maxfd = nb;
			} else {
				FD_SET(m_listen_sock, &rfds);
				if (m_listen_sock > maxfd) maxfd = m_listen_sock;
			}
			struct timeval tv = {};
			tv.tv_sec = 1;
			int s = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
			if (s < 0) { if (errno == EINTR) continue; break; }

			// Drop a pending client that never qualifies (a real host sends a
			// frame within ms): keeps a silent bare-connect from indefinitely
			// occupying the pending slot and blocking a later genuine reconnect.
			if (nb >= 0 && (esp_timer_get_time() - nb_since_us) > 10LL * 1000 * 1000) {
				ESP_LOGW(TAG, "pending NCP client silent >10s -- dropping");
				::close(nb); nb = -1; nb_pending_de = false;
			}

			// (a) New connection while none pending -> accept into the pending
			//     slot + hand it the boot frames, but do NOT publish it as the
			//     active client (m_tcp_sock) until it qualifies.
			if (nb < 0 && FD_ISSET(m_listen_sock, &rfds)) {
				struct sockaddr_in c2 = {};
				socklen_t l2 = sizeof(c2);
				int a = ::accept(m_listen_sock, reinterpret_cast<struct sockaddr*>(&c2), &l2);
				if (a >= 0) {
					int one = 1;
					setsockopt(a, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
					setsockopt(a, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
					int ki = 15, kv = 3, kc = 3;
					setsockopt(a, IPPROTO_TCP, TCP_KEEPIDLE, &ki, sizeof(ki));
					setsockopt(a, IPPROTO_TCP, TCP_KEEPINTVL, &kv, sizeof(kv));
					setsockopt(a, IPPROTO_TCP, TCP_KEEPCNT, &kc, sizeof(kc));
					struct timeval st = {};
					st.tv_sec = 8;
					setsockopt(a, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
					::send(a, boot_ack, sizeof(boot_ack), 0);
					::send(a, ncp_reset_ready, sizeof(ncp_reset_ready), 0);
					nb = a; nb_pending_de = false; nb_since_us = esp_timer_get_time();
					ESP_LOGI(TAG, "second NCP client pending (awaiting DE-AD to preempt)");
				}
			}

			// (b) Pending client sent data -> qualify on DE-AD, else keep waiting.
			if (nb >= 0 && FD_ISSET(nb, &rfds)) {
				int r = ::recv(nb, m_tcp_temp_buf, BUF_SIZE, 0);
				if (r <= 0) {
					::close(nb); nb = -1; nb_pending_de = false;   // gave up
				} else {
					int hit = scan_dead(m_tcp_temp_buf, r, nb_pending_de);
					if (hit != -2) {
						ESP_LOGW(TAG, "pending NCP client qualified -- preempting active client");
						int promoted = nb; nb = -1;
						teardown_active();
						cs = promoted;
						// hit==-1: the 0xDE was in the previous (discarded) chunk;
						// tell rx_qualified_pump a 0xDE preceded so it re-injects
						// it. Otherwise the DE-AD is in this buffer and it re-finds.
						m_tcp_pending_de = (hit == -1);
						{ utils::sem_lock l(m_tcp_fd_sem); m_tcp_sock = cs; }
						rx_qualified_pump(IFACE_TCP, m_tcp_pending_de, m_tcp_temp_buf, r);
						continue;
					}
				}
			}

			// (c) Active client traffic (normal path, behaviour unchanged).
			if (FD_ISSET(cs, &rfds)) {
				int r = ::recv(cs, m_tcp_temp_buf, BUF_SIZE, 0);
				if (r <= 0) {
					// Active client gone. Promote a waiting pending client
					// (unqualified) so the session continues — it qualifies later
					// on its own DE-AD — instead of dropping it to the backlog.
					if (nb >= 0) {
						teardown_active();
						cs = nb; nb = -1;
						m_tcp_pending_de = nb_pending_de;
						{ utils::sem_lock l(m_tcp_fd_sem); m_tcp_sock = cs; }
						continue;
					}
					break;
				}
				rx_qualified_pump(IFACE_TCP, m_tcp_pending_de, m_tcp_temp_buf, r);
			}
		}
		if (nb >= 0) { ::close(nb); nb = -1; }

		// Teardown under m_tcp_fd_sem: write_int may be mid-send on this fd
		// (bounded by the per-frame deadline) — closing under the lock means
		// the fd can never be reused by a fresh accept (or the link
		// watchdog's probe socket) while a stale snapshot of it is still
		// being written to. NOTE: no m_output_buf drain here (an earlier
		// revision had one) — the drain raced the app task as a second
		// stream-buffer reader and orphaned queued EVENT_OUTPUT sizes, eating
		// the NEXT session's first commands. Leftover old-session bytes are
		// harmless instead: the protocol layer is a streaming reassembler and
		// the link-state reset happens at next qualification anyway.
		{
			utils::sem_lock l(m_tcp_fd_sem);
			m_tcp_sock = -1;
			::close(cs);
		}
		ESP_LOGW(TAG, "NCP client disconnected");

		// If the departed client held the TX route, demote to IFACE_NONE (CAS:
		// don't stomp a concurrent flip to a serial host) so NCP->host frames
		// stop targeting a dead socket and a reconnecting client re-qualifies
		// from a clean slate.
		uint8_t expected = IFACE_TCP;
		m_active.compare_exchange_strong(expected, IFACE_NONE,
		                                 std::memory_order_relaxed,
		                                 std::memory_order_relaxed);

		// Let the app task release session-scoped resources (currently the
		// GET_NETWORK_BACKUP RAM snapshot — 40 KB that would otherwise stay
		// resident on the tight Mode-B heap after an aborted pull). Posted as
		// an event so the release runs on the task that owns the resource.
		app::ctx_t ev = { .event = app::EVENT_TCP_DISCONNECT, .size = 0 };
		app::send_event(ev);
	}
}

void transport::close_tcp_client() {
	transport& t = instance();
	if (!t.m_use_tcp) {
		return;
	}
	utils::sem_lock l(t.m_tcp_fd_sem);
	if (t.m_tcp_sock >= 0) {
		// FIN only — the close itself stays with tcp_task (its recv unblocks
		// on this shutdown and runs the normal teardown). A full ::close from
		// here would race tcp_task's own ::close(cs) on a reused fd.
		::shutdown(t.m_tcp_sock, SHUT_RDWR);
	}
}
