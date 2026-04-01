#include "transport.h"
#include "app.h"
#include "utils.h"

#include <esp_log.h>
#include "sdkconfig.h"

#include <driver/usb_serial_jtag.h>
#include <cstring>

static const char* TAG = "TRNPT";

transport::transport() {}

transport& transport::instance() {
	static transport s_transport;
	return s_transport;
}

esp_err_t transport::write_int(const void *buffer, size_t size)
{
    return (usb_serial_jtag_write_bytes(buffer, size, pdMS_TO_TICKS(RINGBUF_TIMEOUT_MS)) == size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void transport::task_int() {
    app::ctx_t ncp_event = {
        .event = app::EVENT_OUTPUT,
        .size = 0
    };

    while (true) {
        int readed = usb_serial_jtag_read_bytes(m_temp_buf, BUF_SIZE, pdMS_TO_TICKS(10));
        if (readed > 0) {
            ncp_event.size = readed;
            xStreamBufferSend(m_output_buf, m_temp_buf, readed, 0);
            app::send_event(ncp_event);
        }
    }
}

size_t transport::output_receive(void* buffer, size_t size) {
	return xStreamBufferReceive(instance().m_output_buf, buffer, size, pdMS_TO_TICKS(RINGBUF_TIMEOUT_MS));
}

esp_err_t transport::process_input_int(void* buffer, size_t size) {
	auto recv_size = xStreamBufferReceive(m_input_buf, buffer, size, pdMS_TO_TICKS(RINGBUF_TIMEOUT_MS));
    if (recv_size != size) {
        ESP_LOGE(TAG, "Input buffer receive error: size %d expect %d!", recv_size, size);
        return ESP_FAIL;
    } else {
        return write_int(buffer, size);
    }
}

esp_err_t transport::send_int(const void* data, size_t size) {
	if (data == NULL) {
        return ESP_FAIL;
    }

    app::ctx_t ncp_event = {
        .event = app::EVENT_INPUT,
        .size = 0
    };

    int count = RINGBUF_TIMEOUT_MS / 10;
    size_t ret_size = 0;

    while (xStreamBufferSpacesAvailable(m_input_buf) < size) {
        if(count == 0) break;
        vTaskDelay(pdMS_TO_TICKS(10));
        count --;
    }

    if (count == 0) {
        ESP_LOGE(TAG, "input_buf not enough");
        return ESP_FAIL;
    }

    {
    	utils::sem_lock l(m_input_sem);
    	ret_size = xStreamBufferSend(m_input_buf, data, size, 0);
    }
    
    if (ret_size != size) {
        ESP_LOGE(TAG, "input_buf send error: size %d expect %d", ret_size, size);
        return ESP_FAIL;
    } else {
        ncp_event.size = size;
        return app::send_event(ncp_event);
    }
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

    usb_serial_jtag_driver_config_t usb_serial_jtag_config;
    usb_serial_jtag_config.rx_buffer_size = BUF_SIZE * 2;
    usb_serial_jtag_config.tx_buffer_size = BUF_SIZE * 2;
    
    esp_err_t err = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB-JTAG driver already installed. Reusing interface.");
        return ESP_OK; 
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB-JTAG driver: %d", err);
        return err;
    }
    
    return ESP_OK;
}

esp_err_t transport::start_int() {
	if (!m_input_buf) {
		ESP_LOGE(TAG,"need init");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG,"start");
	return (xTaskCreate(&task, "transport", TASK_STACK, this, TASK_PRIORITY, NULL) == pdTRUE) ? ESP_OK : ESP_FAIL;
}
