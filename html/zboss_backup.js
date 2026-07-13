const crc8_table = [
    0xea, 0xd4, 0x96, 0xa8, 0x12, 0x2c, 0x6e, 0x50, 0x7f, 0x41, 0x03, 0x3d, 0x87, 0xb9, 0xfb, 0xc5, 0xa5, 0x9b, 0xd9, 0xe7, 0x5d, 0x63, 0x21, 0x1f,
    0x30, 0x0e, 0x4c, 0x72, 0xc8, 0xf6, 0xb4, 0x8a, 0x74, 0x4a, 0x08, 0x36, 0x8c, 0xb2, 0xf0, 0xce, 0xe1, 0xdf, 0x9d, 0xa3, 0x19, 0x27, 0x65, 0x5b,
    0x3b, 0x05, 0x47, 0x79, 0xc3, 0xfd, 0xbf, 0x81, 0xae, 0x90, 0xd2, 0xec, 0x56, 0x68, 0x2a, 0x14, 0xb3, 0x8d, 0xcf, 0xf1, 0x4b, 0x75, 0x37, 0x09,
    0x26, 0x18, 0x5a, 0x64, 0xde, 0xe0, 0xa2, 0x9c, 0xfc, 0xc2, 0x80, 0xbe, 0x04, 0x3a, 0x78, 0x46, 0x69, 0x57, 0x15, 0x2b, 0x91, 0xaf, 0xed, 0xd3,
    0x2d, 0x13, 0x51, 0x6f, 0xd5, 0xeb, 0xa9, 0x97, 0xb8, 0x86, 0xc4, 0xfa, 0x40, 0x7e, 0x3c, 0x02, 0x62, 0x5c, 0x1e, 0x20, 0x9a, 0xa4, 0xe6, 0xd8,
    0xf7, 0xc9, 0x8b, 0xb5, 0x0f, 0x31, 0x73, 0x4d, 0x58, 0x66, 0x24, 0x1a, 0xa0, 0x9e, 0xdc, 0xe2, 0xcd, 0xf3, 0xb1, 0x8f, 0x35, 0x0b, 0x49, 0x77,
    0x17, 0x29, 0x6b, 0x55, 0xef, 0xd1, 0x93, 0xad, 0x82, 0xbc, 0xfe, 0xc0, 0x7a, 0x44, 0x06, 0x38, 0xc6, 0xf8, 0xba, 0x84, 0x3e, 0x00, 0x42, 0x7c,
    0x53, 0x6d, 0x2f, 0x11, 0xab, 0x95, 0xd7, 0xe9, 0x89, 0xb7, 0xf5, 0xcb, 0x71, 0x4f, 0x0d, 0x33, 0x1c, 0x22, 0x60, 0x5e, 0xe4, 0xda, 0x98, 0xa6,
    0x01, 0x3f, 0x7d, 0x43, 0xf9, 0xc7, 0x85, 0xbb, 0x94, 0xaa, 0xe8, 0xd6, 0x6c, 0x52, 0x10, 0x2e, 0x4e, 0x70, 0x32, 0x0c, 0xb6, 0x88, 0xca, 0xf4,
    0xdb, 0xe5, 0xa7, 0x99, 0x23, 0x1d, 0x5f, 0x61, 0x9f, 0xa1, 0xe3, 0xdd, 0x67, 0x59, 0x1b, 0x25, 0x0a, 0x34, 0x76, 0x48, 0xf2, 0xcc, 0x8e, 0xb0,
    0xd0, 0xee, 0xac, 0x92, 0x28, 0x16, 0x54, 0x6a, 0x45, 0x7b, 0x39, 0x07, 0xbd, 0x83, 0xc1, 0xff
];
function crc8(buf) { let crc = 0x00; for(let i=0; i<buf.length; i++) crc = crc8_table[crc ^ buf[i]]; return crc; }

const crc16_table = [
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf, 0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7, 0x1081, 0x0108,
    0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e, 0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876, 0x2102, 0x308b, 0x0210, 0x1399,
    0x6726, 0x76af, 0x4434, 0x55bd, 0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5, 0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e,
    0x54b5, 0x453c, 0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974, 0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3, 0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a, 0xdecd, 0xcf44,
    0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72, 0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9, 0xef4e, 0xfec7, 0xcc5c, 0xddd5,
    0xa96a, 0xb8e3, 0x8a78, 0x9bf1, 0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738, 0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862,
    0x9af9, 0x8b70, 0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7, 0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036, 0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e, 0xa50a, 0xb483,
    0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5, 0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd, 0xb58b, 0xa402, 0x9699, 0x8710,
    0xf3af, 0xe226, 0xd0bd, 0xc134, 0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c, 0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1,
    0xa33a, 0xb2b3, 0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb, 0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a, 0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1, 0x6b46, 0x7acf,
    0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9, 0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330, 0x7bc7, 0x6a4e, 0x58d5, 0x495c,
    0x3de3, 0x2c6a, 0x1ef1, 0x0f78
];
function crc16(buf) { let crc = 0x0000; for(let i=0; i<buf.length; i++) crc = crc16_table[(crc ^ buf[i]) & 0xff] ^ ((crc >> 8) & 0xff); return crc; }

class ZbossSerial {
    constructor(port, logger) {
        this.port = port;
        this.log = logger;
        this.seq = 0;
        // Sequential TSN counter (cycles 1..255..0..1..). Starting at 1 keeps
        // tsn=0 out of the very first request, which some firmwares treat as
        // a "no-tsn" sentinel. Sequential beats Math.random() because the
        // birthday-paradox collision rate over a 320-chunk backup is ~36% with
        // random — sequential only ever wraps once and the wrapped slot is
        // already retired by then.
        this.nextTsn = 1;
        this.reader = null;
        this.writer = null;
        this.buffer = new Uint8Array(0);
        // pendingRequests[tsn] = { cmdId, resolve, reject, timer }. Storing cmdId
        // lets processBuffer reject unsolicited INDICATION frames that
        // happen to land on the same tsn but carry a different cmdId.
        this.pendingRequests = {};
        this.running = false;
        this.writeLock = Promise.resolve();
    }

    async connect() {
        await this.port.open({ baudRate: 115200 });
        // ESP32-C6 USB-Serial-JTAG can interpret certain default DTR/RTS line
        // states asserted by the host at open() time as a reset request. Force
        // both lines low immediately so we don't reboot the chip mid-session
        // (especially nasty during the post-restore auto-identify, where the
        // chip is already in early-boot and a second reset triggers Break-
        // receive on the read stream).
        try {
            await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
        } catch (e) { /* setSignals unsupported — keep going */ }
        this.writer = this.port.writable.getWriter();
        this.reader = this.port.readable.getReader();
        this.running = true;
        this.closed = false;
        this.readLoop();
        this.log("Connected to ESP32-C6.");
    }

    async disconnect() {
        // Idempotent: safe to call multiple times. Without this, a readLoop
        // that died from a transient Break leaves the port handle in 'open'
        // state — the next port.open() then rejects with InvalidStateError.
        if (this.closed) return;
        this.closed = true;
        this.running = false;
        // Reject any still-pending requests so callers don't hang on the
        // 5 s timeout when we already know the link is gone.
        for (const tsn in this.pendingRequests) {
            const slot = this.pendingRequests[tsn];
            try { clearTimeout(slot.timer); } catch (e) {}
            // H3: actually reject the pending promise. Without this, an
            // `await sendCommand(...)` in flight when the link drops (e.g. a
            // transient Break on C6 first-open-after-reenum) never settles and
            // freezes the UI (worst case: mid-restore, after the erase).
            try { if (slot.reject) slot.reject(new Error("link closed")); } catch (e) {}
        }
        this.pendingRequests = {};
        try { if (this.reader) await this.reader.cancel(); } catch (e) {}
        try { if (this.writer) this.writer.releaseLock(); } catch (e) {}
        try { await this.port.close(); } catch (e) {}
        this.log("Disconnected.");
    }

    async readLoop() {
        while (this.running) {
            try {
                const { value, done } = await this.reader.read();
                if (done) break;
                if (value) {
                    const newBuf = new Uint8Array(this.buffer.length + value.length);
                    newBuf.set(this.buffer);
                    newBuf.set(value, this.buffer.length);
                    this.buffer = newBuf;
                    this.processBuffer();
                }
            } catch (e) {
                if (this.running) this.log("Read error: " + e.message);
                break;
            }
        }
        // Loop terminated — proactively close the port so it doesn't leak
        // into a stuck 'open' state. Subsequent re-opens then succeed.
        if (!this.closed) {
            await this.disconnect();
        }
    }

    processBuffer() {
        while (this.buffer.length >= 7) {
            let start = -1;
            for (let i = 0; i < this.buffer.length - 1; i++) {
                if (this.buffer[i] === 0xDE && this.buffer[i+1] === 0xAD) { start = i; break; }
            }
            if (start === -1) {
                // JSHOST-4: no DE AD pair found. A 0xDE in the LAST position may
                // be the first byte of a signature whose 0xAD arrives in the next
                // read (frame split across reads) — keep it, drop everything
                // else. The old code discarded the whole buffer including that
                // 0xDE, desyncing on a read boundary that split the signature.
                // Mirrors the firmware find_signature() trailing-0xDE behavior.
                const last = this.buffer.length - 1;
                this.buffer = (last >= 0 && this.buffer[last] === 0xDE)
                    ? this.buffer.slice(last)
                    : new Uint8Array(0);
                return;
            }
            if (start > 0) this.buffer = this.buffer.slice(start);
            if (this.buffer.length < 7) return;

            const packet_len = this.buffer[2] | (this.buffer[3] << 8);
            // Mirror firmware protocol.cpp (C2b): a valid frame is packet_len
            // === 5 (empty / ACK) or >= 7 (header + 2-byte data CRC + >=1
            // payload byte). Values in {0,1,2,3,4,6} are malformed; skip the
            // DE AD and resync instead of consuming a bogus length. Keeps the
            // two parsers byte-compatible.
            if (packet_len !== 5 && packet_len < 7) {
                this.buffer = this.buffer.slice(2);
                continue;
            }
            // JSHOST-6: validate the 1-byte header CRC8 (over bytes 2..5) BEFORE
            // trusting packet_len to frame the rest. On mismatch skip the DE AD
            // and resync, exactly like the firmware (protocol.cpp header-CRC path).
            if (crc8(this.buffer.slice(2, 6)) !== this.buffer[6]) {
                this.buffer = this.buffer.slice(2);
                continue;
            }
            if (this.buffer.length < packet_len + 2) return; // Wait for full frame

            const frame = this.buffer.slice(0, packet_len + 2);
            this.buffer = this.buffer.slice(packet_len + 2);

            const isACK = (frame[5] & 0x01) === 1;
            const isNACK = (frame[5] & 0x02) === 0x02;
            const sequence = (frame[5] >> 2) & 0x03;
            const ackSeq = (frame[5] >> 4) & 0x03;
            this.recvSeq = sequence;

            // JSHOST-7: a device NACK (our host→device frame failed its CRC on
            // the chip) was silently ignored, so a pending request only failed
            // via the 5 s timeout. Surface it. The NACK carries ackSeq (the
            // packet_seq it rejects), not a tsn, so we can't map it to a specific
            // request — log it and let the per-request timeout fire. Don't ACK a
            // NACK (it sets both is_ack and is_nack); retransmit is unsupported
            // on the device side anyway (protocol.cpp on_rx_packet).
            if (isNACK) {
                this.log("Device NACK received (ackSeq=" + ackSeq + "); a request frame was rejected, will time out.");
                continue;
            }

            if (isACK && packet_len === 5) {
                // Ignore empty ACKs
                continue;
            }

            if (packet_len > 5) {
                const payload = frame.slice(9); // Skip header (7) and crc16 (2)
                // ACK every received frame regardless of whether we route it
                // — the chip's flow control depends on us acknowledging.
                this.sendAck(this.recvSeq);

                // JSHOST-6: validate the payload CRC16 (frame[7..8], LE) before
                // delivering. A corrupted payload must not resolve a pending
                // request with garbage; ACK first (flow control), then drop on
                // mismatch so the request times out and the caller can retry.
                const expectedCrc = frame[7] | (frame[8] << 8);
                const actualCrc = crc16(payload);
                if (actualCrc !== expectedCrc) {
                    this.log("RX payload CRC16 mismatch (got 0x" + actualCrc.toString(16) +
                             " want 0x" + expectedCrc.toString(16) + "), dropping frame.");
                    continue;
                }

                const cmdId = payload[2] | (payload[3] << 8);
                const tsn = payload[4];
                const slot = this.pendingRequests[tsn];
                // Two-key match: tsn + cmdId. The chip can emit unsolicited
                // INDICATION frames (APSDE/ZDP, especially while a network is
                // up and routers/end-devices are still on air) that carry
                // their own tsn — without the cmdId check, an INDICATION
                // whose tsn happens to collide with our pending request
                // would resolve that promise with garbage. The chip's actual
                // response (with the same tsn, but matching cmdId) would
                // then arrive too late and get dropped.
                if (slot && slot.cmdId === cmdId) {
                    clearTimeout(slot.timer);
                    slot.resolve(payload.slice(5));
                    delete this.pendingRequests[tsn];
                }
            }
        }
    }

    async sendAck(ackSeq) {
        const header = new Uint8Array(7);
        header[0] = 0xDE; header[1] = 0xAD;
        header[2] = 5; header[3] = 0; header[4] = 0x06;
        header[5] = 0xC1 | ((ackSeq & 0x03) << 4);
        header[6] = crc8(header.slice(2, 6));
        this.writeLock = this.writeLock.then(() => this.writer.write(header)).catch(e => this.log(e));
        await this.writeLock;
    }

    async sendCommand(cmdId, payload) {
        const tsn = this.nextTsn;
        this.nextTsn = (this.nextTsn + 1) & 0xFF;
        const dataLen = 5 + payload.length; // version, type, cmdId(2), tsn, payload
        const reqData = new Uint8Array(dataLen);
        reqData[0] = 0; // version
        reqData[1] = 0; // REQ
        reqData[2] = cmdId & 0xFF;
        reqData[3] = (cmdId >> 8) & 0xFF;
        reqData[4] = tsn;
        reqData.set(payload, 5);

        const packetLen = 7 + dataLen;
        const out = new Uint8Array(2 + packetLen);
        out[0] = 0xDE; out[1] = 0xAD;
        out[2] = packetLen & 0xFF; out[3] = (packetLen >> 8) & 0xFF;
        out[4] = 0x06; // type
        out[5] = 0xC0 | ((this.seq & 0x03) << 2) | (((this.recvSeq || 0) & 0x03) << 4);
        out[6] = crc8(out.slice(2, 6));

        const crc = crc16(reqData);
        out[7] = crc & 0xFF; out[8] = (crc >> 8) & 0xFF;
        out.set(reqData, 9);

        this.seq = (this.seq + 1) & 0x03;

        return new Promise(async (resolve, reject) => {
            const timer = setTimeout(() => { delete this.pendingRequests[tsn]; reject("Timeout (cmd=0x" + cmdId.toString(16) + " tsn=" + tsn + ")"); }, 5000);
            this.pendingRequests[tsn] = { cmdId, resolve, reject, timer };
            this.writeLock = this.writeLock.then(() => this.writer.write(out)).catch(e => this.log(e));
            await this.writeLock;
        });
    }
}

// ---------------------------------------------------------------------------
// Universal NWK Backup helpers — produce the same coordinator_backup.json
// shape that the Z2M-fork's zigbee-herdsman adapter:zboss writes, so the file
// is interchangeable between web-flasher backups and Z2M backups.
//
// Wire formats:
//  - GET_STRUCTURED_BACKUP (0x009B) response payload (after cmdHdr):
//        category(1) status(1) magic(4 LE) version(1) flags(1) payload_len(2 LE) tlvs[payload_len]
//    Magic = 'ZBSB' = 0x4253425A. TLVs: tag(1) length(2 LE) value(length).
//    Tags: 0x01 pan_id(u16 LE), 0x02 ext_pan_id(8B), 0x03 channel(u8),
//          0x04 nwk_update_id(u8), 0x05 coordinator_ieee(8B), 0x06 nwk_key(16B),
//          0x07 nwk_key_seq(u8), 0x08 frame_counter(u32 LE),
//          0x10 device_table (N x 16B = ieee[8] short[2 LE] flags[1] reserved[5]).
//  - GET_NETWORK_BACKUP (0x0099) response payload (after cmdHdr):
//        category(1) status(1) total_size(u32 LE) chunk_length(u32 LE) data[chunk_length]
// ---------------------------------------------------------------------------

function buf2hex(buf) {
    return Array.from(buf).map(b => b.toString(16).padStart(2, '0')).join('');
}

function buf2base64(buf) {
    let s = '';
    const chunk = 0x8000;
    for (let i = 0; i < buf.length; i += chunk) {
        s += String.fromCharCode.apply(null, buf.subarray(i, Math.min(i + chunk, buf.length)));
    }
    return btoa(s);
}

function base642buf(s) {
    const bin = atob(s);
    const buf = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
    return buf;
}

function parseStructuredTLVs(payload) {
    if (payload[1] !== 0) throw new Error("GET_STRUCTURED_BACKUP failed (status=" + payload[1] + ")");
    const dv = new DataView(payload.buffer, payload.byteOffset);
    const magic = dv.getUint32(2, true);
    if (magic !== 0x4253425A) throw new Error("Bad magic in structured backup (0x" + magic.toString(16) + ")");
    const version = payload[6];
    if (version !== 1) throw new Error("Unsupported structured-backup format version: " + version);
    const payload_len = dv.getUint16(8, true);
    const tlvs = payload.slice(10, 10 + payload_len);

    const result = { devices: [] };
    let p = 0;
    while (p + 3 <= tlvs.length) {
        const tag = tlvs[p];
        const tlen = tlvs[p+1] | (tlvs[p+2] << 8);
        p += 3;
        const val = tlvs.slice(p, p + tlen);
        p += tlen;
        switch (tag) {
            case 0x01: result.pan_id = val[0] | (val[1] << 8); break;
            case 0x02: result.extended_pan_id = val; break;
            case 0x03: result.channel = val[0]; break;
            case 0x04: result.nwk_update_id = val[0]; break;
            case 0x05: result.coordinator_ieee = val; break;
            case 0x06: result.network_key = val; break;
            case 0x07: result.network_key_seq = val[0]; break;
            case 0x08: result.frame_counter = new DataView(val.buffer, val.byteOffset, val.byteLength).getUint32(0, true); break;
            case 0x10: {
                for (let i = 0; i + 16 <= val.length; i += 16) {
                    const short = val[i+8] | (val[i+9] << 8);
                    result.devices.push({
                        nwk_address: short.toString(16).padStart(4, '0'),
                        ieee_address: buf2hex(val.slice(i, i + 8)),
                        is_child: true,
                    });
                }
                break;
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Identify — pulls a set of read-only getters and populates the
// "Coordinator Info" card. Non-destructive; safe to call on a live coord.
//
// Wire-format reference (from main/commands_impl.h):
//   GET_MODULE_VERSION       (0x0001) resp: 3 x uint32 LE
//                                    fwVersion is firmware-encoded as
//                                    (MAJOR<<24)|(MINOR<<16)|(BUILD & 0xFFFF)
//                                    — note BUILD is 16-bit, NOT split into
//                                    revision/commit bytes the way herdsman's
//                                    generic ver2str() decodes it.
//   GET_COORDINATOR_VERSION  (0x0024) resp: uint8
//   GET_LOCAL_IEEE_ADDR      (0x000b) req: uint8 (mac index, =0); resp: uint8 + 8B IEEE
//   GET_ZIGBEE_ROLE          (0x0004) resp: uint8 (0=coord, 1=router, 2=ed, 3=none)
//   GET_JOINED               (0x0014) resp: uint8 (0/1)
//   GET_PAN_ID               (0x0009) resp: uint16 LE
//   GET_EXTENDED_PAN_ID      (0x0023) resp: 8B (firmware-reversed to match wire)
//   GET_ZIGBEE_CHANNEL       (0x0008) resp: uint8 page + uint8 channel
//   GET_STRUCTURED_BACKUP    (0x009B) — for device count + live frame counter
// GET_TX_POWER (0x0010) is commented out in commands_impl.h — returns
// NOT_IMPLEMENTED, so we skip it.
// ---------------------------------------------------------------------------

const ROLE_NAMES = { 0: "Coordinator", 1: "Router", 2: "End device", 3: "None" };

function formatIEEE(bytes /* Uint8Array(8) */) {
    return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join(':');
}

function formatFwVersion(u32) {
    const major = (u32 >>> 24) & 0xff;
    const minor = (u32 >>> 16) & 0xff;
    const build = u32 & 0xffff;
    return `v${major}.${minor}.${build}`;
}

function formatStackVersion(u32) {
    const a = (u32 >>> 24) & 0xff;
    const b = (u32 >>> 16) & 0xff;
    const c = (u32 >>> 8) & 0xff;
    const d = u32 & 0xff;
    return `${a}.${b}.${c}.${d}`;
}

function parseSemverV(s) {
    // Accepts "v1.2.32" or "1.2.32", returns [major, minor, build] or null.
    const m = /^v?(\d+)\.(\d+)\.(\d+)/.exec(s || '');
    return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
}
function compareSemver(a, b) {
    for (let i = 0; i < 3; i++) {
        if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

// Modal helpers — used to show a friendly waiting overlay during the
// post-restore window when the chip is rebooting.

function showRebootModal(title, msg) {
    const m = document.getElementById('reboot-modal');
    if (!m) return;
    const t = document.getElementById('reboot-modal-title');
    const x = document.getElementById('reboot-modal-msg');
    const c = document.getElementById('reboot-modal-countdown');
    if (t) t.textContent = title || 'Chip is rebooting…';
    if (x) x.textContent = msg || 'Please wait — re-enumerating over USB.';
    if (c) c.textContent = '';
    m.style.display = 'flex';
}
function setRebootModalCountdown(secondsLeft) {
    const c = document.getElementById('reboot-modal-countdown');
    if (c) c.textContent = secondsLeft > 0 ? `${secondsLeft}s` : '…';
}
function hideRebootModal() {
    const m = document.getElementById('reboot-modal');
    if (m) m.style.display = 'none';
    const cd = document.getElementById('reboot-modal-countdown');
    if (cd) cd.style.display = '';
    const sp = document.getElementById('reboot-modal-spinner');
    if (sp) sp.style.display = '';
    const btn = document.getElementById('reboot-modal-button-wrap');
    if (btn) btn.style.display = 'none';
}
// Switch the modal from countdown mode into "ready, click to refresh" mode.
function showRebootModalReady() {
    const t = document.getElementById('reboot-modal-title');
    const x = document.getElementById('reboot-modal-msg');
    const cd = document.getElementById('reboot-modal-countdown');
    const sp = document.getElementById('reboot-modal-spinner');
    const btnWrap = document.getElementById('reboot-modal-button-wrap');
    if (t) t.textContent = 'Chip should be ready';
    if (x) x.textContent = 'Click Refresh to re-read the network state from the freshly-restored ESP32-C6.';
    if (cd) cd.style.display = 'none';
    if (sp) sp.style.display = 'none';
    if (btnWrap) btnWrap.style.display = 'block';
}
async function modalRefreshAndIdentify() {
    hideRebootModal();
    await identifyCoordinator();
}
async function waitWithCountdown(seconds, title, msg) {
    showRebootModal(title, msg);
    for (let s = seconds; s > 0; s--) {
        setRebootModalCountdown(s);
        await new Promise(r => setTimeout(r, 1000));
    }
    setRebootModalCountdown(0);
}

// Hard wall-clock timeout around an arbitrary promise. WebSerial port.open()
// is documented to be able to hang indefinitely on some Chrome+Linux+ESP32-C6
// combinations after the chip's USB-Serial-JTAG re-enumerated. Without an
// outer guard the modal countdown finishes but the identify attempt that
// follows never resolves nor rejects — the modal stays on screen forever.
function withTimeout(promise, ms, label) {
    return Promise.race([
        promise,
        new Promise((_, reject) => setTimeout(
            () => reject(new Error(`${label} timed out after ${ms / 1000}s`)),
            ms
        )),
    ]);
}

// Identify helpers — extracted so both the user-triggered identifyCoordinator()
// and the post-restore auto-refresh can reuse them.

function _resetInfoCard() {
    const setField = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
    ['info-fw', 'info-coord', 'info-stack', 'info-ieee', 'info-role',
     'info-joined', 'info-pan', 'info-extpan', 'info-channel',
     'info-devices', 'info-fc'].forEach(id => setField(id, '—'));
    const statusEl = document.getElementById('info-status-line');
    if (statusEl) { statusEl.style.display = 'none'; statusEl.className = ''; statusEl.innerHTML = ''; }
    const flashWrap = document.getElementById('info-flash-wrap');
    if (flashWrap) flashWrap.style.display = 'none';
    const card = document.getElementById('info-card');
    if (card) card.style.display = 'grid';
}

// Outer retry — Chrome WebSerial on Linux + ESP32-C6 USB-Serial-JTAG very
// reliably emits a transient Break-receive on the FIRST port.open() after the
// chip's USB phy re-enumerated (i.e. after esp_restart from RESTORE_NETWORK
// or just a fresh USB plug). The second attempt always works cleanly. So we
// try once, and on failure auto-retry once after a short settle delay — the
// user only sees one logical Identify operation.
async function _identifyOnPort(port, logger) {
    for (let attempt = 1; attempt <= 2; attempt++) {
        if (attempt > 1) {
            logger("First attempt hit a transient USB break — retrying in 1s…");
            await new Promise(r => setTimeout(r, 1000));
        }
        const ok = await _identifyOnPortSingle(port, logger);
        if (ok) return true;
    }
    return false;
}

async function _identifyOnPortSingle(port, logger) {
    const setField = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
    const showStatus = (cls, html) => {
        const el = document.getElementById('info-status-line');
        if (!el) return;
        el.className = cls;
        el.innerHTML = html;
        el.style.display = 'block';
    };
    const showFlashAction = (label) => {
        const wrap = document.getElementById('info-flash-wrap');
        const btn = document.getElementById('info-flash-btn');
        if (wrap && btn) {
            btn.textContent = label;
            wrap.style.display = 'block';
        }
    };

    let identifiedFw = null;
    let zboss = null;
    let ok = false;
    try {
        zboss = new ZbossSerial(port, logger);
        await zboss.connect();

        logger("Identifying coordinator…");

        // Module version — 12 B payload after status (cat+stat at [0..1]).
        const r1 = await zboss.sendCommand(0x0001 /* GET_MODULE_VERSION */, new Uint8Array(0));
        if (r1[1] !== 0) throw new Error("GET_MODULE_VERSION failed");
        const dv1 = new DataView(r1.buffer, r1.byteOffset);
        const fwVer = dv1.getUint32(2, true);
        const stackVer = dv1.getUint32(6, true);
        const protoVer = dv1.getUint32(10, true);
        identifiedFw = formatFwVersion(fwVer);
        ok = true;
        setField('info-fw', identifiedFw);
        setField('info-stack', `ZBOSS ${formatStackVersion(stackVer)} · Protocol ${formatStackVersion(protoVer)}`);

        // Coordinator version (single uint8).
        const r2 = await zboss.sendCommand(0x0024 /* GET_COORDINATOR_VERSION */, new Uint8Array(0));
        if (r2[1] === 0) setField('info-coord', `busware.de ESP32 (ZBOSS) · NCP rev ${r2[2]}`);

        // Local IEEE — arg=0 (primary mac). Firmware now sends LSB-first (wire/LE
        // order, issue #6 fix); reverse for canonical MSB-first display.
        const r3 = await zboss.sendCommand(0x000b /* GET_LOCAL_IEEE_ADDR */, new Uint8Array([0]));
        if (r3[1] === 0) setField('info-ieee', formatIEEE(r3.slice(3, 11).reverse()));

        // Role.
        const r4 = await zboss.sendCommand(0x0004 /* GET_ZIGBEE_ROLE */, new Uint8Array(0));
        if (r4[1] === 0) setField('info-role', ROLE_NAMES[r4[2]] || ("Unknown(" + r4[2] + ")"));

        // Joined — informational only. The chip can return joined=0 during
        // the boot-phase window between NVRAM-load and BDB-reattach, even
        // though the NIB already has live values. So we don't gate the other
        // queries on this — try them all unconditionally below.
        const r5 = await zboss.sendCommand(0x0014 /* GET_JOINED */, new Uint8Array(0));
        const joined = (r5[1] === 0 && r5[2] === 1);
        setField('info-joined', joined ? 'Joined' : 'Not joined');

        // PAN (uint16 LE).
        try {
            const r6 = await zboss.sendCommand(0x0009 /* GET_PAN_ID */, new Uint8Array(0));
            if (r6[1] === 0) {
                const pan = r6[2] | (r6[3] << 8);
                setField('info-pan', pan === 0xFFFF ? '— (no network)' : '0x' + pan.toString(16).padStart(4, '0'));
            }
        } catch (e) { logger("GET_PAN_ID failed: " + e); }

        // Extended PAN (8 bytes, firmware-reversed to canonical hex order).
        try {
            const r7 = await zboss.sendCommand(0x0023 /* GET_EXTENDED_PAN_ID */, new Uint8Array(0));
            if (r7[1] === 0) {
                const allZero = r7.slice(2, 10).every(b => b === 0);
                const allFF   = r7.slice(2, 10).every(b => b === 0xFF);
                setField('info-extpan', (allZero || allFF) ? '— (no network)' : formatIEEE(r7.slice(2, 10)));
            }
        } catch (e) { logger("GET_EXTENDED_PAN_ID failed: " + e); }

        // Channel (page + channel).
        try {
            const r8 = await zboss.sendCommand(0x0008 /* GET_ZIGBEE_CHANNEL */, new Uint8Array(0));
            if (r8[1] === 0) setField('info-channel', r8[3] === 0xFF ? '— (no network)' : `${r8[3]} (page ${r8[2]})`);
        } catch (e) { logger("GET_ZIGBEE_CHANNEL failed: " + e); }

        // Structured backup — gives device count + live frame counter.
        try {
            const r9 = await zboss.sendCommand(0x009B /* GET_STRUCTURED_BACKUP */, new Uint8Array(0));
            const s = parseStructuredTLVs(r9);
            setField('info-devices', `${s.devices.length} paired`);
            setField('info-fc', s.frame_counter != null ? String(s.frame_counter) : '—');
        } catch (e) {
            logger("GET_STRUCTURED_BACKUP failed: " + e);
        }

        await zboss.disconnect();
        logger("Identify done.");

        // Compare against manifest.json's latest version + give the user
        // a contextual next-step recommendation.
        try {
            const m = await fetch("manifest.json", { cache: "no-store" });
            const mj = await m.json();
            const latest = mj.version || "";
            const latestSem = parseSemverV(latest);
            const haveSem = parseSemverV(identifiedFw);
            // "Has network state worth preserving" — not the GET_JOINED flag
            // (which can be stale during boot) but the actual PAN field. If
            // PAN shows a real value (not '— (no network)'), we treat the
            // chip as carrying a paired network.
            const panEl = document.getElementById('info-pan');
            const hasNetwork = panEl && panEl.textContent.startsWith('0x');

            if (latestSem && haveSem) {
                const cmp = compareSemver(haveSem, latestSem);
                if (cmp >= 0) {
                    showStatus('uptodate',
                        `✓ Firmware is up to date (${latest}).` +
                        (hasNetwork ? ' This chip carries a paired network — take a backup before any reflash.' : ''));
                } else {
                    showStatus('outdated',
                        `ℹ Newer firmware available: ${latest} (you have ${identifiedFw}).` +
                        (hasNetwork
                            ? ' <strong>Take a backup first</strong>, then flash, then restore.'
                            : ' This chip is not on a network — safe to flash directly.'));
                    showFlashAction(`Update firmware to ${latest}`);
                }
            } else {
                showStatus('uptodate', `Firmware: ${identifiedFw}. Latest available: ${latest || '(unknown)'}.`);
            }
        } catch (e) {
            // manifest fetch failed — silently skip the version comparison.
        }
    } catch (e) {
        logger("Identify error: " + e);
        // If the chip didn't respond at all, treat it as a factory candidate
        // and offer a clean flash path.
        if (!identifiedFw) {
            showStatus('factory',
                'ℹ The connected device did not respond to NCP queries. ' +
                'This usually means it is either factory-fresh or running a different firmware.');
            showFlashAction('Flash factory firmware');
        }
    } finally {
        // Always release the port. ZbossSerial.disconnect() is idempotent —
        // safe even if readLoop already closed it on a Break-receive.
        if (zboss) {
            try { await zboss.disconnect(); } catch (e) {}
        }
    }
    return ok;
}

async function identifyCoordinator() {
    const logger = (msg) => document.getElementById('log').innerText += msg + '\n';
    let port;
    try {
        port = await navigator.serial.requestPort();
    } catch (e) {
        logger("Error: " + e);
        return;
    }
    _resetInfoCard();
    await _identifyOnPort(port, logger);
}

async function doBackup() {
    const logger = (msg) => document.getElementById('log').innerText += msg + '\n';
    const progress = document.getElementById('progress');
    let zboss = null;
    try {
        const port = await navigator.serial.requestPort();
        zboss = new ZbossSerial(port, logger);
        await zboss.connect();

        progress.style.display = 'block';
        progress.innerText = 'Reading network identity…';

        // Step 1: structured backup (TLV with PAN/ExtPAN/key/frame-counter/devices).
        // Non-fatal on failure — raw NVRAM below is the authoritative restore source.
        logger("Requesting structured identity (0x009B)…");
        let structured = null;
        try {
            const r = await zboss.sendCommand(155 /* GET_STRUCTURED_BACKUP */, new Uint8Array(0));
            structured = parseStructuredTLVs(r);
            logger(`  pan=0x${(structured.pan_id || 0).toString(16)} ch=${structured.channel} key=${structured.network_key ? 'yes' : 'no'} fc=${structured.frame_counter} devs=${structured.devices.length}`);
        } catch (e) {
            logger("  structured-identity unavailable (" + e + "), backup will be raw-only");
        }

        // Step 2: raw NVRAM (chunked, ~40 KB total).
        logger("Reading raw NVRAM (chunked)…");
        let offset = 0;
        let totalSize = 0;
        const chunks = [];

        do {
            const payload = new Uint8Array(4);
            new DataView(payload.buffer).setUint32(0, offset, true);
            const response = await zboss.sendCommand(153 /* GET_NETWORK_BACKUP */, payload);
            if (response[1] !== 0) throw new Error("GET_NETWORK_BACKUP failed at offset " + offset);
            const rdv = new DataView(response.buffer, response.byteOffset);
            totalSize = rdv.getUint32(2, true);
            const chunkLen = rdv.getUint32(6, true);
            if (chunkLen > 0) {
                chunks.push(response.slice(10, 10 + chunkLen));
                offset += chunkLen;
                progress.innerText = `Backup: ${Math.round((offset/totalSize)*100)}%`;
            } else {
                break;
            }
        } while (offset < totalSize);

        const raw = new Uint8Array(totalSize);
        let ptr = 0;
        for (const c of chunks) { raw.set(c, ptr); ptr += c.length; }

        await zboss.disconnect();
        logger(`Backup complete: ${totalSize} bytes raw NVRAM` + (structured ? ` + structured identity` : ``));

        // Step 3: build the Universal NWK Backup JSON shape (Z2M-compatible).
        // Hybrid: structured fields for inspection + raw_nvram for byte-true restore.
        const backup = {
            metadata: {
                format: "zigpy/open-coordinator-backup",
                version: 1,
                source: "esp-coordinator-webflasher",
                internal: { date: new Date().toISOString() },
            },
            stack_specific: {
                zboss: { raw_nvram: buf2base64(raw) },
            },
        };
        if (structured) {
            if (structured.coordinator_ieee) backup.coordinator_ieee = buf2hex(structured.coordinator_ieee);
            if (structured.pan_id !== undefined) backup.pan_id = structured.pan_id.toString(16).padStart(4, '0');
            if (structured.extended_pan_id) backup.extended_pan_id = buf2hex(structured.extended_pan_id);
            if (structured.nwk_update_id !== undefined) backup.nwk_update_id = structured.nwk_update_id;
            backup.security_level = 5;
            if (structured.channel !== undefined) {
                backup.channel = structured.channel;
                backup.channel_mask = [structured.channel];
            }
            if (structured.network_key) {
                backup.network_key = {
                    key: buf2hex(structured.network_key),
                    sequence_number: structured.network_key_seq || 0,
                    frame_counter: structured.frame_counter || 0,
                };
            }
            backup.devices = structured.devices;
        }

        const blob = new Blob([JSON.stringify(backup, null, 2)], { type: "application/json" });
        const link = document.createElement("a");
        link.href = URL.createObjectURL(blob);
        const dateStr = new Date().toISOString().slice(0, 10);
        link.download = `coordinator_backup_${dateStr}.json`;
        link.click();
        progress.innerText = `Saved: ${link.download}`;
    } catch (e) {
        logger("Error: " + e);
    } finally {
        // JSHOST-3: always release the port. disconnect() is idempotent, so this
        // is a no-op after the success-path disconnect above, but it closes the
        // port if connect()/a command threw mid-backup — otherwise the handle
        // leaks and the next port.open() rejects with InvalidStateError.
        if (zboss) { try { await zboss.disconnect(); } catch (e) {} }
    }
}

async function doRestore() {
    const logger = (msg) => document.getElementById('log').innerText += msg + '\n';
    const progress = document.getElementById('progress');
    const input = document.getElementById('restoreFile');
    if (!input.files || input.files.length === 0) {
        alert("Please select a backup file first!");
        return;
    }

    let port;
    try {
        // Must be called immediately on user action before any other awaits.
        port = await navigator.serial.requestPort();
    } catch (e) {
        logger("Error: " + e);
        return;
    }

    const file = input.files[0];
    const buffer = await file.arrayBuffer();
    let raw;

    // Auto-detect: Universal NWK Backup JSON (coordinator_backup.json) vs legacy raw .bin.
    const head = new TextDecoder('utf-8', { fatal: false }).decode(buffer.slice(0, Math.min(buffer.byteLength, 256))).trimStart();
    if (head.startsWith('{')) {
        try {
            const json = JSON.parse(new TextDecoder('utf-8').decode(buffer));
            const b64 = json && json.stack_specific && json.stack_specific.zboss && json.stack_specific.zboss.raw_nvram;
            if (!b64) throw new Error("backup JSON has no stack_specific.zboss.raw_nvram");
            raw = base642buf(b64);
            logger(`Detected Universal NWK Backup JSON (${raw.length} bytes raw NVRAM` + (json.pan_id ? `, pan=0x${json.pan_id} ch=${json.channel}` : ``) + `)`);
        } catch (e) {
            logger("Failed to parse backup JSON: " + (e.message || e));
            return;
        }
    } else {
        raw = new Uint8Array(buffer);
        logger(`Detected legacy raw NVRAM binary (${raw.length} bytes)`);
    }

    const totalSize = raw.length;
    // JSHOST-5: RESTORE_NETWORK erases nvs+zb_storage on the offset-0 chunk.
    // Reject a wrong-sized image up front (the firmware also rejects
    // total_size != capacity before erasing, but failing here is clearer and
    // never opens a session). 0xA000 = nvs(24K) + zb_storage(16K).
    const EXPECTED_RAW_SIZE = 0x6000 + 0x4000;
    if (totalSize !== EXPECTED_RAW_SIZE) {
        logger(`Refusing restore: raw NVRAM is ${totalSize} bytes, expected ${EXPECTED_RAW_SIZE} (0x${EXPECTED_RAW_SIZE.toString(16)}). File is truncated or not an esp-coordinator backup.`);
        return;
    }

    let zboss = null;
    try {
        zboss = new ZbossSerial(port, logger);
        await zboss.connect();

        logger("Starting restore…");
        let offset = 0;
        const chunkSize = 128;
        progress.style.display = 'block';

        while (offset < totalSize) {
            const end = Math.min(offset + chunkSize, totalSize);
            const chunk = raw.slice(offset, end);

            const payload = new Uint8Array(8 + chunk.length);
            const dv = new DataView(payload.buffer);
            dv.setUint32(0, offset, true);
            dv.setUint32(4, totalSize, true);
            payload.set(chunk, 8);

            const response = await zboss.sendCommand(154 /* RESTORE_NETWORK */, payload);
            if (response[1] !== 0) throw new Error("Restore failed at offset " + offset);

            offset += chunk.length;
            progress.innerText = `Restore: ${Math.round((offset/totalSize)*100)}%`;
        }

        logger("Restore complete! The ESP32 is rebooting.");
        await zboss.disconnect();

        // Two-phase reboot-wait modal:
        //   1) Countdown 10 s while the chip's USB-Serial-JTAG detaches + ROM
        //      reboot + ZBOSS stack init runs. The user can see progress.
        //   2) Replace the countdown with a "Refresh" button. Click it and
        //      identifyCoordinator() runs against a freshly user-gestured
        //      requestPort() — same code path that manual Identify uses,
        //      which is what reliably works post-reboot. (Auto-retry via the
        //      same SerialPort reference is fragile because Chrome WebSerial's
        //      port.open() can hang for tens of seconds after the underlying
        //      OS device went away + came back, even with everything closed
        //      on our side. The user gesture sidesteps that.)
        progress.innerText = '';
        _resetInfoCard();
        await waitWithCountdown(10, 'Chip is rebooting…', 'Waiting for the ESP32-C6 to re-enumerate over USB. When the countdown ends, click "Refresh now" to read the new network state.');
        showRebootModalReady();
    } catch (e) {
        logger("Error: " + e);
        hideRebootModal();
    } finally {
        // JSHOST-3: idempotent — releases the port if the restore loop threw
        // (e.g. a chunk NACK) so the handle doesn't leak past a failed restore
        // and block the next port.open().
        if (zboss) { try { await zboss.disconnect(); } catch (e) {} }
    }
}

window.doBackup = doBackup;
window.doRestore = doRestore;
window.identifyCoordinator = identifyCoordinator;
window.modalRefreshAndIdentify = modalRefreshAndIdentify;
