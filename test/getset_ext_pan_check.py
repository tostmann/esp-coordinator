#!/usr/bin/env python3
# GETSET-3 hardware SET->GET round-trip check (Group C, 2026-05-30).
#
# Drives the ZBOSS NCP serial protocol directly against an esp-coordinator
# running the Group-C firmware (v1.2.33). Verifies, on real hardware:
#   - GET_MODULE_VERSION (0x0001)        -> confirm the flashed build
#   - GET_EXTENDED_PAN_ID (0x0023)       -> baseline (reads nwkExtendedPANId, reversed on the wire)
#   - GET_STRUCTURED_BACKUP (0x009B)     -> TAG_EXT_PAN_ID (0x02), an INDEPENDENT raw read path (no reversal)
#   - SET_EXTENDED_PAN_ID (0x0033)       -> writes apsUseExtendedPANID (NOW byte-reversed by the GETSET-3 fix)
#   - GET_EXTENDED_PAN_ID again          -> did SET change GET?
#
# Interpretation:
#   Y1 == X            -> SET<->GET round-trips (same attribute + byte order correct)
#   Y1 == reverse(X)   -> byte-order regression
#   Y1 == Y0 (!= X)    -> SET did NOT affect GET => set/get target different attributes
#                         (apsUseExtendedPANID vs nwkExtendedPANId) — the documented caveat.
#
# Framing is byte-identical to test/smoke_structured_backup.py / html/zboss_backup.js.
# Usage: python3 getset_ext_pan_check.py /dev/ttyACM5
import sys, time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM5"

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

def build_request(cmd_id, payload=b"", tsn=0x42, seq=0, ack_seq=0):
    inner = bytes([0, 0, cmd_id & 0xFF, (cmd_id >> 8) & 0xFF, tsn]) + payload
    packet_len = 7 + len(inner)
    flags = 0xC0 | ((seq & 0x03) << 2) | ((ack_seq & 0x03) << 4)
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

def txn(ser, cmd_id, payload=b"", tsn=0x42, timeout_s=2.0):
    """Send one REQUEST, return the (category, status, body_after_status) of the
    matching non-ACK RESPONSE for cmd_id, or None."""
    ser.reset_input_buffer()
    ser.write(build_request(cmd_id, payload, tsn=tsn))
    for f in read_frames(ser, timeout_s):
        flags = f[5]
        if flags & 0x01:        # ACK frame, skip
            continue
        if len(f) < 9 + 5:
            continue
        pl = f[9:]              # skip 7B header + 2B crc16
        rcmd = pl[2] | (pl[3] << 8)
        if rcmd != cmd_id:
            continue
        body = pl[5:]           # after cmd_t (version,type,cmd_lo,cmd_hi,tsn)
        if len(body) < 2:
            return None
        return body[0], body[1], body[2:]
    return None

def hx(b): return ":".join(f"{x:02x}" for x in b)

def main():
    print(f"== GETSET-3 HW SET->GET check on {PORT} ==")
    # Open with DTR/RTS LOW (matches html/zboss_backup.js): the ESP32-C6
    # USB-Serial-JTAG treats asserted DTR/RTS at open() as a reset request and
    # will otherwise hold the chip in reset. Opening still pulses a reset, so
    # wait out the ~2-3s boot and drain the boot-ready (NCP_RESET tsn=0xFF) frame.
    ser = serial.Serial()
    ser.port = PORT; ser.baudrate = 115200; ser.timeout = 0.1
    ser.dtr = False; ser.rts = False
    ser.open()
    time.sleep(0.3)
    read_frames(ser, 4.0)  # let it boot, drain unsolicited boot frame

    # 1. firmware version
    r = txn(ser, 0x0001, tsn=0x01)
    if r and r[1] == 0 and len(r[2]) >= 4:
        v = int.from_bytes(r[2][0:4], "little")
        print(f"[GET_MODULE_VERSION] fwVersion=0x{v:08x} -> {(v>>24)&0xff}.{(v>>16)&0xff}.{v&0xffff}")
    else:
        print(f"[GET_MODULE_VERSION] unexpected: {r}")

    # 2. baseline GET
    r0 = txn(ser, 0x0023, tsn=0x10)
    y0 = bytes(r0[2][:8]) if (r0 and r0[1] == 0 and len(r0[2]) >= 8) else None
    print(f"[GET_EXTENDED_PAN_ID #0] status={r0[1] if r0 else '?'} value={hx(y0) if y0 else None}")

    # 3. structured backup ext-pan (independent RAW read path, no reversal)
    rs = txn(ser, 0x009B, tsn=0x11, timeout_s=3.0)
    sb_ext = None
    if rs and rs[1] == 0:
        img = rs[2]
        if len(img) >= 8 and img[0:4] == b"ZBSB":
            plen = img[6] | (img[7] << 8); tlvs = img[8:8+plen]; p = 0
            while p + 3 <= len(tlvs):
                tag = tlvs[p]; tl = tlvs[p+1] | (tlvs[p+2] << 8); p += 3
                val = tlvs[p:p+tl]; p += tl
                if tag == 0x02 and tl == 8:
                    sb_ext = bytes(val)
    print(f"[GET_STRUCTURED_BACKUP TAG_EXT_PAN_ID] value={hx(sb_ext) if sb_ext else None}  (raw path, no reversal)")

    # 4. SET a distinctive non-palindrome
    X = bytes([0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88])
    rset = txn(ser, 0x0033, payload=X, tsn=0x12)
    print(f"[SET_EXTENDED_PAN_ID] sent X={hx(X)} -> status={rset[1] if rset else '?'} "
          f"({'OK' if rset and rset[1]==0 else 'NOT OK'})")

    # 5. GET again
    r1 = txn(ser, 0x0023, tsn=0x13)
    y1 = bytes(r1[2][:8]) if (r1 and r1[1] == 0 and len(r1[2]) >= 8) else None
    print(f"[GET_EXTENDED_PAN_ID #1] status={r1[1] if r1 else '?'} value={hx(y1) if y1 else None}")

    print("\n== ANALYSIS ==")
    if y1 is None:
        print("  GET#1 failed — inconclusive."); sys.exit(2)
    if y1 == X:
        print("  Y1 == X  -> SET<->GET ROUND-TRIPS. set/get share storage; byte order correct.")
    elif y1 == X[::-1]:
        print("  Y1 == reverse(X)  -> BYTE-ORDER REGRESSION (the GETSET-3 fix is wrong somewhere).")
    elif y0 is not None and y1 == y0:
        print("  Y1 == Y0 (unchanged, != X)  -> SET did NOT affect GET.")
        print("  => CONFIRMS the caveat: zb_set_extended_pan_id writes apsUseExtendedPANID,")
        print("     zb_get_extended_pan_id reads nwkExtendedPANId — different attributes.")
        print("     SET->GET cannot validate the byte-order fix on a formed network.")
    else:
        print(f"  Y1={hx(y1)} — unexpected; neither X, reverse(X), nor unchanged baseline.")
    print(f"\n  Y0(baseline)={hx(y0) if y0 else None}")
    print(f"  X(sent)      ={hx(X)}")
    print(f"  Y1(after SET)={hx(y1) if y1 else None}")
    print(f"  structured   ={hx(sb_ext) if sb_ext else None}")

if __name__ == "__main__":
    main()
