#include "protocol.h"
#include "transport.h"
#include "app.h"
#include "utils.h"

#include <esp_log.h>
#include <cstring>

static const char* TAG = "PROT";



protocol::protocol() {

}

protocol& protocol::instance() {
	static protocol s_protocol;
	return s_protocol;
}

static uint8_t* find_signature(uint8_t* s,const uint8_t* end) {
	while (true) {
		if (s == end) return nullptr;
		if (*s++ == 0xde)
			break;
	}
	if (s == end)
		return nullptr;
	if (*s == 0xad)
		return s-1;
	return nullptr;
}

static const uint8_t next_seq_map[4] = {
	0x01,
	0x02,
	0x03,
	0x01
};
static uint8_t next_seq(uint8_t seq) {
	return next_seq_map[seq & 0x03];
}

void protocol::send_ack(const ncp_header_t& hdr) {
	ncp_header_t rsp = {
		.signature = {0xde,0xad},
		.packet_len = 5,
		.packet_type = ZBOSS_NCP_API_HL,
		.is_ack = 1,
		.is_nack = 0,
		.packet_seq = m_tx_seq,
		.ack_seq = hdr.packet_seq,
		.first_fragment = 1,
		.last_fragment = 1,
		.header_crc = 0
	};
	m_tx_seq = next_seq(m_tx_seq);
	rsp.header_crc = utils::crc8(&rsp.packet_len,4);
	auto res = transport::send(&rsp,sizeof(rsp));
	if (res != ESP_OK) {
		ESP_LOGE(TAG,"Failed send ACK");
	}
}

void protocol::send_nack(const ncp_header_t& hdr) {
	ncp_header_t rsp = {
		.signature = {0xde,0xad},
		.packet_len = 5,
		.packet_type = ZBOSS_NCP_API_HL,
		.is_ack = 1,
		.is_nack = 1,
		.packet_seq = m_tx_seq,
		.ack_seq = hdr.packet_seq,
		.first_fragment = 1,
		.last_fragment = 1,
		.header_crc = 0
	};
	m_tx_seq = next_seq(m_tx_seq);
	rsp.header_crc = utils::crc8(&rsp.packet_len,4);
	auto res = transport::send(&rsp,sizeof(rsp));
	if (res != ESP_OK) {
		ESP_LOGE(TAG,"Failed send NACK");
	}
}

esp_err_t protocol::send_data_int(const void* data,size_t size) {
	if (!data || size==0) {
		return ESP_OK; // @todo
	}
	if (size > (TX_BUFFER_SIZE-sizeof(ncp_header_t)-2)) {
		ESP_LOGE(TAG,"failed send data, too long");
		return ESP_FAIL;
	}

	utils::sem_lock l(m_tx_sem);
	
	auto hdr = reinterpret_cast<ncp_header_t*>(m_tx_buffer);
	hdr->signature[0] = 0xde;
	hdr->signature[1] = 0xad;
	hdr->packet_len = size + sizeof(ncp_header_t) + 2 - 2;
	hdr->packet_type = ZBOSS_NCP_API_HL;
	hdr->is_ack = 0;
	hdr->is_nack = 0;
	hdr->packet_seq = m_tx_seq;
	hdr->ack_seq = 0;
	hdr->first_fragment = 1;
	hdr->last_fragment = 1;
	hdr->header_crc = utils::crc8(&hdr->packet_len,4);
	m_tx_seq = next_seq(m_tx_seq);
	*reinterpret_cast<uint16_t*>(hdr+1) = utils::crc16(data,size);
	memcpy(reinterpret_cast<uint8_t*>(hdr+1)+2,data,size);
	auto data_size = sizeof(ncp_header_t) + 2 + size;
	return transport::send(m_tx_buffer,data_size);
}

void protocol::on_rx_packet(const ncp_header_t& hdr,const void* data,size_t data_size) {
	if (hdr.is_ack) {
		// ack received
	} 
	if (hdr.is_nack) {
		ESP_LOGE(TAG,"NACK received, retransmit not supported");
		return;
	}
	if (!data)
		return;
	if (hdr.packet_type != ZBOSS_NCP_API_HL) {
		ESP_LOGE(TAG,"invalid packet type: %02x",int(hdr.packet_type));
		send_nack(hdr);
		return;
	}
	if (!hdr.is_ack) {
		send_ack(hdr);
	}
	app::on_rx_data(data,data_size);
}

esp_err_t protocol::on_rx_int(const void* data,size_t size) {
	if (m_rx_buffer_pos + size > RX_BUFFER_SIZE) {
		ESP_LOGE(TAG,"Buffer full, skip part");
		auto overflow = (m_rx_buffer_pos + size) - RX_BUFFER_SIZE;
		if (overflow < m_rx_buffer_pos) {
			memmove(m_rx_buffer,&m_rx_buffer[overflow],m_rx_buffer_pos-overflow);
		}
		m_rx_buffer_pos = RX_BUFFER_SIZE-size;
	}
	memcpy(&m_rx_buffer[m_rx_buffer_pos],data,size);
	m_rx_buffer_pos += size;
	auto rx_data_size = m_rx_buffer_pos;
	auto rx_data = m_rx_buffer;
	auto end = &m_rx_buffer[m_rx_buffer_pos];

	while (rx_data_size >= 7) {
		
		auto p = find_signature(rx_data,end);
		if (p) {
			if (p != rx_data) {
				ESP_LOGD(TAG,"Skip data %d",p-rx_data);
			}
			rx_data = p;
			rx_data_size = end-p;

			if (rx_data_size < 7) {
				break; // need more data
			}
			auto hdr = reinterpret_cast<const ncp_header_t*>(rx_data);
			auto hdr_crc = utils::crc8(&hdr->packet_len,4);
			if (hdr_crc != hdr->header_crc) {
				rx_data += 2;
				rx_data_size -= 2;
				ESP_LOGE(TAG,"Invalid header crc %02x/%02x",int(hdr->header_crc),int(hdr_crc));
				continue;	
			}
			if (rx_data_size < (hdr->packet_len+2)) { // 
				break; // need more data
			}
			size_t packet_len;
			if (hdr->packet_len == 5) {
				packet_len = sizeof(ncp_header_t);
				// empty packet
				on_rx_packet(*hdr,nullptr,0);
				
			} else {
				size_t data_len = hdr->packet_len - 5 - 2;
				packet_len = sizeof(ncp_header_t) + 2 + data_len;
				auto data_crc_expected = *reinterpret_cast<const uint16_t*>(hdr+1);
				auto data = reinterpret_cast<const uint8_t*>(hdr+1) + 2;
				
				auto data_crc = utils::crc16(data,data_len);
				if (data_crc != data_crc_expected) {
					ESP_LOGE(TAG,"Invalid data crc %04x/%04x",int(data_crc_expected),int(data_crc));
					send_nack(*hdr);
				} else {
					// packet with data
					on_rx_packet(*hdr,data,data_len);
				}
			}
			rx_data += packet_len;
			rx_data_size -= packet_len;
		} else {
			break; // need more data
		}
	}
	if (rx_data != m_rx_buffer && rx_data_size) {
		memmove(m_rx_buffer,rx_data,rx_data_size); // tail
	}
	m_rx_buffer_pos = rx_data_size;
	return ESP_OK;
}



esp_err_t protocol::init_int() {
	ESP_LOGI(TAG,"init");
	m_rx_buffer_pos = 0;
	m_tx_seq = 0;
	m_tx_sem = xSemaphoreCreateMutex();
    if (!m_tx_sem) {
        ESP_LOGE(TAG, "Input semaphore create error");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t protocol::start_int() {
	return ESP_OK;
}

