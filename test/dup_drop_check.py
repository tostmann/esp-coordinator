#!/usr/bin/env python3
# DUP-1 hardware check: link-layer duplicate-drop of retransmitted host data
# frames (main/protocol.cpp on_rx_packet).
#
# Wire scenario (mirrors zigpy-zboss >= 2.0.0 / zigbee-herdsman retry logic:
# stable packet_seq + RETRANSMIT flag on ACK timeout):
#   T1  GET_MODULE_VERSION, seq=1, no flag      -> expect ACK + 1 response
#   T2  byte-identical frame, RETRANSMIT flag   -> expect ACK + NO response
#       (lost-ACK case: re-ACK, drop)
#   T3  GET_MODULE_VERSION, seq=2, no flag      -> expect ACK + 1 response
#       (link continues normally after a dup)
#   T4  GET_MODULE_VERSION, seq=3, RETRANSMIT   -> expect ACK + 1 response
#       (lost-FRAME case: flagged retransmission of a frame we never saw =
#       first delivery, must dispatch)
#
# Pre-DUP-1 firmware drops T2/T4 entirely (no ACK, no dispatch) — the early
# `if (hdr.is_nack) return;` treated the data-frame retransmit bit as NACK.
#
# Framing byte-identical to getset_ext_pan_check.py / html/zboss_backup.js.
# Usage: python3 dup_drop_check.py /dev/ttyACM4
import sys, time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM4"

CRC8_TABLE = bytes([
    0xea,0xd4,0x96,0xa8,0x12,0x2c,0x6e,0x50,0x7f,0x41,0x03,0x3d,0x87,0xb9,0xfb,0xc5,
    0xa5,0x9b,0xd9,0xe7,0x5d,0x63,0x21,0x1f,0x30,0x0e,0x4c,0x72,0xc8,0xf6,0xb4,0x8a,
    0x74,0x4a,0x08,0x36,0x8c,0xb2,0xf0,0xce,0xe1,0xdf,0x9d,0xa3,0x19,0x27,0x65,0x5b,
    0x3b,0x05,0x47,0x79,0xc3,0xfd,0xbf,0x81,0xae,0x90,0xd2,0xec,0x56,0x68,0x2a,0x14,
    0xb3,0x8d,0xcf,0xf1,0x4b,0x75,0x37,0x09,0x26,0x18,0x5a,0x64,0xde,0xe0,0xa2,0x9c,
    0xfc,0xc2,0x80,0xbe,0x04,0x3a,0x78,0x46,0x69,0x57,0x15,0x2b,0x91,0xaf,0xed,0xd3,
    0x2d,0x13,0x51,0x6f,0xd5,0xeb,0xa9,0x97,0xb8,0x86,0xc4,0xfa,0x40,0x7e,0x3c,0x02,
    0x62,0x5c,0x1e,0x20,0x9a,0xa4,0xe6,0xd8,0xf7,0xc9,0x8b,0xb5,0x0f,0x31,0x73,0x4d,
    0x58,0x66,0x24,0x1a,0xa0,0x9e,0xdc,0xe2,0xcd,0xf3,0xb1,0x8f,0x35,0x0b,0x49,0x77,
    0x17,0x29,0x6b,0x55,0xef,0xd1,0x93,0xad,0x82,0xbc,0xfe,0xc0,0x7a,0x44,0x06,0x38,
    0xc6,0xf8,0xba,0x84,0x3e,0x00,0x42,0x7c,0x53,0x6d,0x2f,0x11,0xab,0x95,0xd7,0xe9,
    0x89,0xb7,0xf5,0xcb,0x71,0x4f,0x0d,0x33,0x1c,0x22,0x60,0x5e,0xe4,0xda,0x98,0xa6,
    0x01,0x3f,0x7d,0x43,0xf9,0xc7,0x85,0xbb,0x94,0xaa,0xe8,0xd6,0x6c,0x52,0x10,0x2e,
    0x4e,0x70,0x32,0x0c,0xb6,0x88,0xca,0xf4,0xdb,0xe5,0xa7,0x99,0x23,0x1d,0x5f,0x61,
    0x9f,0xa1,0xe3,0xdd,0x67,0x59,0x1b,0x25,0x0a,0x34,0x76,0x48,0xf2,0xcc,0x8e,0xb0,
    0xd0,0xee,0xac,0x92,0x28,0x16,0x54,0x6a,0x45,0x7b,0x39,0x07,0xbd,0x83,0xc1,0xff,
])

def crc8(buf):
    crc = 0
    for b in buf:
        crc = CRC8_TABLE[crc ^ b]
    return crc

def crc16(buf):
    crc = 0
    for b in buf:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc & 0xFFFF

GET_MODULE_VERSION = 0x0001

def build_request(cmd_id, payload=b"", tsn=0x42, seq=0, ack_seq=0, retransmit=False):
    inner = bytes([0, 0, cmd_id & 0xFF, (cmd_id >> 8) & 0xFF, tsn]) + payload
    packet_len = 7 + len(inner)
    flags = 0xC0 | ((seq & 0x03) << 2) | ((ack_seq & 0x03) << 4)
    if retransmit:
        flags |= 0x02
    hdr = bytearray(7)
    hdr[0] = 0xDE; hdr[1] = 0xAD
    hdr[2] = packet_len & 0xFF; hdr[3] = (packet_len >> 8) & 0xFF
    hdr[4] = 0x06
    hdr[5] = flags
    hdr[6] = crc8(hdr[2:6])
    c = crc16(inner)
    return bytes(hdr) + bytes([c & 0xFF, (c >> 8) & 0xFF]) + inner

def read_frames(ser, timeout_s=2.0):
    end = time.time() + timeout_s
    buf = bytearray()
    while time.time() < end:
        chunk = ser.read(256)
        if chunk: buf += chunk
    frames = []
    i = 0
    while i + 7 <= len(buf):
        if buf[i] != 0xDE or buf[i+1] != 0xAD:
            i += 1; continue
        plen = buf[i+2] | (buf[i+3] << 8)
        end_off = i + 2 + plen
        if end_off > len(buf):
            break
        frames.append(bytes(buf[i:end_off]))
        i = end_off
    return frames

def classify(frames, cmd_id):
    """Return (n_acks, n_responses_for_cmd, other) over a frame list."""
    acks = resp = other = 0
    for f in frames:
        flags = f[5]
        if flags & 0x01:
            acks += 1
            continue
        if len(f) >= 9 + 5:
            pl = f[9:]
            rcmd = pl[2] | (pl[3] << 8)
            if rcmd == cmd_id:
                resp += 1
                continue
        other += 1
    return acks, resp, other

def exchange(ser, frame, timeout_s=2.0):
    ser.reset_input_buffer()
    ser.write(frame)
    return read_frames(ser, timeout_s)

def main():
    print(f"== DUP-1 duplicate-drop check on {PORT} ==")
    ser = serial.Serial()
    ser.port = PORT; ser.baudrate = 115200; ser.timeout = 0.1
    ser.dtr = False; ser.rts = False
    ser.open()
    print("-- opened (DTR/RTS low); waiting out boot + draining boot frames")
    time.sleep(3.0)
    boot = read_frames(ser, 2.0)
    print(f"-- drained {len(boot)} boot frame(s)")

    results = []

    # T1: normal command, seq=1
    f1 = build_request(GET_MODULE_VERSION, tsn=0x10, seq=1)
    a, r, o = classify(exchange(ser, f1), GET_MODULE_VERSION)
    ok1 = (a >= 1 and r == 1)
    results.append(("T1 normal seq=1            -> ACK + 1 response", a, r, ok1))

    # T2: byte-identical retransmission, RETRANSMIT flag, same seq, same tsn
    f2 = build_request(GET_MODULE_VERSION, tsn=0x10, seq=1, retransmit=True)
    a, r, o = classify(exchange(ser, f2), GET_MODULE_VERSION)
    ok2 = (a >= 1 and r == 0)
    results.append(("T2 dup seq=1 +RT (lost-ACK) -> ACK + 0 responses", a, r, ok2))

    # T3: next normal command, seq=2
    f3 = build_request(GET_MODULE_VERSION, tsn=0x11, seq=2)
    a, r, o = classify(exchange(ser, f3), GET_MODULE_VERSION)
    ok3 = (a >= 1 and r == 1)
    results.append(("T3 normal seq=2            -> ACK + 1 response", a, r, ok3))

    # T4: flagged retransmission of a frame we never delivered (fresh seq)
    f4 = build_request(GET_MODULE_VERSION, tsn=0x12, seq=3, retransmit=True)
    a, r, o = classify(exchange(ser, f4), GET_MODULE_VERSION)
    ok4 = (a >= 1 and r == 1)
    results.append(("T4 fresh seq=3 +RT (lost-frame) -> ACK + 1 response", a, r, ok4))

    print()
    all_ok = True
    for label, a, r, ok in results:
        print(f"{'PASS' if ok else 'FAIL'}  {label}   [acks={a} responses={r}]")
        all_ok = all_ok and ok
    print()
    print(f"== RESULT: {'ALL PASS' if all_ok else 'FAILURES PRESENT'} ==")
    ser.close()
    sys.exit(0 if all_ok else 1)

main()
