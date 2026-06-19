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

// Return the first plausible signature start: a 0xDE that is either followed by
// 0xAD (a real DE AD) or sits at the very end of the buffer (a partial signature
// to keep while waiting for more bytes). PROTO-2: the previous version returned
// nullptr on a 0xDE followed by a non-0xAD byte, so the caller broke and never
// advanced past that false 0xDE — the buffer kept the stale prefix and resync
// stalled until overflow. Now a false 0xDE is skipped and the scan continues, so
// e.g. `DE FF DE AD …` still locks onto the real signature at index 2.
static uint8_t* find_signature(uint8_t* s,const uint8_t* end) {
	while (s != end) {
		if (*s == 0xde) {
			if (s + 1 == end)   // 0xDE at buffer end: keep it, wait for more data
				return s;
			if (s[1] == 0xad)   // full DE AD signature
				return s;
			// false 0xDE (next byte != 0xAD): fall through to skip it
		}
		++s;
	}
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
	// m_tx_seq is shared: this ACK path runs on the app task (on_rx_packet /
	// on_rx_int) while send_data_int runs on the ZBOSS task and send_nack on the
	// app task too. send_data_int already serialises its read-modify-write under
	// m_tx_sem; this path historically did NOT, so the two writers could read the
	// same seq and emit two frames carrying a DUPLICATE sequence number (lost
	// update) — the host then logs "Unexpected packet sequence" and may drop a
	// frame. Hold m_tx_sem across the seq assignment AND transport::send so the
	// frame is both uniquely numbered and queued in sequence order, exactly like
	// send_data_int. (transport.cpp H2 named this race but only guarded m_input_buf;
	// the m_tx_seq counter was left unprotected until now.) Lock order is always
	// m_tx_sem -> m_input_sem (the latter taken inside transport::send), matching
	// send_data_int, so no deadlock is introduced.
	utils::sem_lock l(m_tx_sem);
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
	// Same m_tx_seq race as send_ack — serialise the RMW + send under m_tx_sem.
	utils::sem_lock l(m_tx_sem);
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
	// PROTO-3: hdr+1 sits at offset sizeof(ncp_header_t)==7 (odd) — an unaligned
	// uint16_t store is UB. memcpy the CRC16 instead (compiles to the same on C6).
	uint16_t data_crc = utils::crc16(data,size);
	memcpy(reinterpret_cast<uint8_t*>(hdr+1),&data_crc,sizeof(data_crc));
	memcpy(reinterpret_cast<uint8_t*>(hdr+1)+2,data,size);
	auto data_size = sizeof(ncp_header_t) + 2 + size;
	return transport::send(m_tx_buffer,data_size);
}

void protocol::on_rx_packet(const ncp_header_t& hdr,const void* data,size_t data_size) {
	if (hdr.is_ack) {
		// ACK (or, with the retransmit bit set, NACK) from the host for one
		// of OUR frames. Retransmitting our frames is not implemented.
		if (hdr.is_nack) {
			ESP_LOGE(TAG,"NACK received, retransmit not supported");
		}
		return;
	}
	if (!data)
		return;
	if (hdr.packet_type != ZBOSS_NCP_API_HL) {
		ESP_LOGE(TAG,"invalid packet type: %02x",int(hdr.packet_type));
		send_nack(hdr);
		return;
	}
	// DUP-1: on a DATA frame the is_nack bit position is the spec's
	// RETRANSMIT flag — the host re-sent this frame because it did not see
	// our ACK (or the original frame was lost on the wire). zigpy-zboss
	// >= 2.0.0 and zigbee-herdsman both retransmit with a STABLE packet_seq
	// + the retransmit flag on a ~1 s ACK timeout. Pre-DUP-1 behaviour was
	// the worst of both worlds: the early `if (hdr.is_nack) return;` above
	// dropped every flagged retransmission WITHOUT re-ACK and WITHOUT
	// dispatch, so a host retry could never succeed (and an unflagged
	// duplicate would have executed the command twice).
	// Now: already-accepted seq -> re-ACK (the lost-ACK case) but do NOT
	// dispatch again; unseen seq -> first delivery (the lost-frame case),
	// accept normally.
	if (hdr.is_nack && m_last_rx_seq_valid && hdr.packet_seq == m_last_rx_seq) {
		ESP_LOGW(TAG,"duplicate data frame seq=%d (retransmit) — re-ACK, drop",int(hdr.packet_seq));
		send_ack(hdr);
		return;
	}
	send_ack(hdr);
	m_last_rx_seq = hdr.packet_seq;
	m_last_rx_seq_valid = true;
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
				if (hdr->packet_len < 7) {
					// packet_len in {0,1,2,3,4,6}: too short to carry the 2-byte
					// data CRC + >=1 payload byte (5 was the empty-packet case
					// above). Without this guard `packet_len - 5 - 2` underflows
					// to ~4 GB and crc16() walks off m_rx_buffer -> LoadProhibited
					// reboot from one crafted frame. Skip the DE AD and resync,
					// exactly like the header-CRC-mismatch path above (C2).
					rx_data += 2;
					rx_data_size -= 2;
					continue;
				}
				size_t data_len = hdr->packet_len - 5 - 2;
				packet_len = sizeof(ncp_header_t) + 2 + data_len;
				// PROTO-3: unaligned read at offset 7 — memcpy instead of an
				// unaligned uint16_t load (UB; works on C6 SRAM).
				uint16_t data_crc_expected;
				memcpy(&data_crc_expected, reinterpret_cast<const uint8_t*>(hdr+1), sizeof(data_crc_expected));
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
	// Start at 1, not 0 — the hardcoded boot-time raw_data ACK and NCP_RESET
	// response in app::start_int() are sent via transport directly (bypassing
	// this protocol layer) and both carry packet_seq=0 hardcoded. If the
	// first dynamic packet were also seq=0, the host would see three packets
	// from us with the same seq before any advancement, which some host
	// implementations treat as duplicate-drop. next_seq() then cycles
	// 1->2->3->1 (seq 0 only used at boot).
	m_tx_seq = 1;
	// DUP-1: no host data frame accepted yet — first frame after boot is
	// fresh regardless of its seq. NOTE for the wifi-coex merge: the TCP
	// transport's reset_link_state() must ALSO clear these two on a fresh
	// accept, or a stale seq from the previous TCP session could eat the
	// first flagged retransmission of the new one.
	m_last_rx_seq = 0;
	m_last_rx_seq_valid = false;
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

void protocol::reset_link_state_int() {
	// A wifi-coex Mode B TCP client reconnect is a brand-new host process whose
	// packet sequence restarts at 0. Re-baseline our side to the same post-boot
	// state init_int() sets up: drop any half-reassembled inbound frame and
	// restart m_tx_seq at 1 (seq 0 stays reserved for the raw boot ACK / NCP_RESET
	// frames the transport re-emits on each accept). Without this, m_tx_seq keeps
	// climbing across the disconnect, so the next session's first frame carries a
	// stale seq and herdsman logs "Unexpected packet sequence" for the whole link.
	//
	// Called from the TCP task at fresh-accept time, while m_tcp_sock is still -1
	// (so any concurrent send_data is dropped, not leaked to the new client).
	// m_rx_buffer_pos is owned by the app task in on_rx_int; the transport drains
	// m_output_buf on disconnect and the app task has emptied its queue well before
	// a new client connects, so the lock-free reset here cannot tear a concurrent
	// on_rx_int (size_t store is atomic on RISC-V32; worst case a stale partial
	// frame survives one accept and resyncs via find_signature). The m_tx_seq
	// write IS serialised under m_tx_sem so it cannot lose to a spontaneous
	// INDICATION's send_data_int RMW racing in the publish window.
	m_rx_buffer_pos = 0;
	// DUP-1: clear the duplicate-drop baseline too (mirrors init_int). The
	// previous TCP session's last accepted packet_seq must not survive into the
	// next one — the fresh host's seq restarts at 0, and a stale match here would
	// drop its first flagged retransmission as a duplicate instead of delivering
	// it. Same single-byte-store / app-task-quiescent safety as the
	// m_rx_buffer_pos reset above, so no lock is needed.
	m_last_rx_seq = 0;
	m_last_rx_seq_valid = false;
	{
		utils::sem_lock l(m_tx_sem);
		m_tx_seq = 1;
	}
}

