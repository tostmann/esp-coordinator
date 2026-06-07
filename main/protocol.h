#pragma once
#include <cstdint>
#include <esp_err.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class protocol {
private:
	struct ncp_header_t {
		uint8_t signature[2];
		uint16_t packet_len;
		uint8_t packet_type;
		uint8_t is_ack: 1;
		// On the wire this bit is the spec's RETRANSMIT flag. Its meaning
		// depends on is_ack: ACK+retransmit = NACK ("resend frame ack_seq");
		// DATA+retransmit = "this data frame is a retransmission" (host did
		// not see our ACK, or the original frame was lost). The legacy field
		// name is_nack only describes the first case — see DUP-1 in
		// protocol.cpp for the data-frame case.
		uint8_t is_nack: 1;
		uint8_t packet_seq: 2;
		uint8_t ack_seq: 2;
		uint8_t first_fragment: 1;
		uint8_t last_fragment: 1;
		uint8_t header_crc;
	} __attribute__((packed));
	static_assert(sizeof(ncp_header_t)==7);

	protocol();
	static protocol& instance();
	esp_err_t init_int();
	esp_err_t start_int();

	static constexpr size_t RX_BUFFER_SIZE = 1024;
	// 2048 covers worst-case ZDO responses with full 64-entry tables:
	// ZDO_MGMT_LQI_REQ -> 64*sizeof(zb_zdo_neighbor_table_record_t) ~ 1.4 KB,
	// ZDO_MGMT_BIND_REQ -> 64*sizeof(zb_zdo_binding_table_record_t) ~ 1.5 KB.
	// Pre-fix size 256 silently rejected anything larger via send_data_int's
	// "too long" guard, so the host never saw populated neighbor / binding
	// tables once the network grew past ~10 nodes.
	static constexpr size_t TX_BUFFER_SIZE = 2048;
	static constexpr uint8_t ZBOSS_NCP_API_HL = 0x06;

	uint8_t m_rx_buffer[RX_BUFFER_SIZE];
	size_t m_rx_buffer_pos;
	uint8_t m_tx_seq;
	// DUP-1 duplicate-drop state: packet_seq of the last ACCEPTED (ACKed +
	// dispatched) host data frame. Only touched on the app task (single
	// consumer of on_rx_packet) — no locking needed. m_last_rx_seq_valid
	// gates the very first frame after boot (any seq is fresh then).
	uint8_t m_last_rx_seq;
	bool m_last_rx_seq_valid;

	uint8_t m_tx_buffer[TX_BUFFER_SIZE];
	SemaphoreHandle_t m_tx_sem;        /*!< A semaphore handle send_data, becouse it use buffer */

	esp_err_t on_rx_int(const void* data,size_t size);
	void on_rx_packet(const ncp_header_t& hdr,const void* data,size_t data_size);
	void send_ack(const ncp_header_t& hdr);
	void send_nack(const ncp_header_t& hdr);
	esp_err_t send_data_int(const void* data,size_t size);
	
public:
	// Largest single frame the protocol layer ever hands to transport::send
	// (NCP->host direction = m_tx_buffer). app::m_buffer must be >= this or an
	// oversize frame (e.g. a full 64-entry Mgmt_Lqi/Mgmt_Bind table) strands in
	// transport's m_input_buf and permanently desyncs the link — see the
	// static_assert in app::process_event (H1).
	static constexpr size_t MAX_FRAME_SIZE = TX_BUFFER_SIZE;

	static esp_err_t init() { return instance().init_int(); }
	static esp_err_t start() { return instance().start_int(); }

	static esp_err_t on_rx(const void* data,size_t size) {
		return instance().on_rx_int(data,size);
	}
	static esp_err_t send_data(const void* data,size_t size) {
		return instance().send_data_int(data,size);
	}
};