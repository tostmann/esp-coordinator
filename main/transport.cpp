#include "transport.h"
#include "app.h"
#include "utils.h"

#include <esp_log.h>
#include "sdkconfig.h"

#include <driver/usb_serial_jtag.h>
#if CONFIG_NCP_UART_TRANSPORT
#include <driver/uart.h>
#include <driver/gpio.h>
#endif
#include <cstring>

static const char* TAG = "TRNPT";

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
    // Dual-interface TX routing (see iface_t in transport.h). Frames go only
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

    if (active != IFACE_UART) {
        const TickType_t ticks = (active == IFACE_USB) ? pdMS_TO_TICKS(RINGBUF_TIMEOUT_MS) : 0;
        if (usb_serial_jtag_write_bytes(buffer, size, ticks) == (int)size) {
            delivered = true;
        } else if (active == IFACE_USB) {
            ESP_LOGE(TAG, "usb write failed (%u bytes)", (unsigned)size);
        }
    }

#if CONFIG_NCP_UART_TRANSPORT
    if (m_uart_ok && active != IFACE_USB) {
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
        // rx_pump only ever runs on our own RX tasks (USB/UART), never on
        // the ZBOSS task (app::send_event itself must stay non-blocking for
        // that caller — see the critical-section panic note there).
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
	ESP_LOGI(TAG,"start");
	if (xTaskCreate(&task, "transport", TASK_STACK, this, TASK_PRIORITY, NULL) != pdTRUE) {
		return ESP_FAIL;
	}
#if CONFIG_NCP_UART_TRANSPORT
	if (m_uart_ok &&
	    xTaskCreate(&uart_task, "transport_uart", TASK_STACK, this, TASK_PRIORITY, NULL) != pdTRUE) {
		return ESP_FAIL;
	}
#endif
	return ESP_OK;
}
