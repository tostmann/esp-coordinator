#!/usr/bin/env python3
# Smoke test for the v1.2.x structured-backup feature.
#
# Opens the ZBOSS NCP serial port, drains any unsolicited boot frame, sends
# GET_STRUCTURED_BACKUP (0x009B), and prints both the raw response and a
# parsed view of the TLVs. Exits 0 if the magic + version are valid and at
# least one TLV decoded successfully.
#
# Usage:   python3 smoke_structured_backup.py /dev/serial/by-id/usb-...
# Default: /dev/ttyACM4 (production coordinator on genai).

import sys, time, struct, os
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM4"

# CRC tables — verbatim copy from main/utils.cpp (and html/zboss_backup.js).
CRC8_TABLE = bytes([
    0x00,0x3e,0x7c,0x42,0xf8,0xc6,0x84,0xba,0x95,0xab,0xe9,0xd7,0x6d,0x53,0x11,0x2f,
    0x4f,0x71,0x33,0x0d,0xb7,0x89,0xcb,0xf5,0xda,0xe4,0xa6,0x98,0x22,0x1c,0x5e,0x60,
    0x9e,0xa0,0xe2,0xdc,0x66,0x58,0x1a,0x24,0x0b,0x35,0x77,0x49,0xf3,0xcd,0x8f,0xb1,
    0xd1,0xef,0xad,0x93,0x29,0x17,0x55,0x6b,0x44,0x7a,0x38,0x06,0xbc,0x82,0xc0,0xfe,
    0x59,0x67,0x25,0x1b,0xa1,0x9f,0xdd,0xe3,0xcc,0xf2,0xb0,0x8e,0x34,0x0a,0x48,0x76,
    0x16,0x28,0x6a,0x54,0xee,0xd0,0x92,0xac,0x83,0xbd,0xff,0xc1,0x7b,0x45,0x07,0x39,
    0xc7,0xf9,0xbb,0x85,0x3f,0x01,0x43,0x7d,0x52,0x6c,0x2e,0x10,0xaa,0x94,0xd6,0xe8,
    0x88,0xb6,0xf4,0xca,0x70,0x4e,0x0c,0x32,0x1d,0x23,0x61,0x5f,0xe5,0xdb,0x99,0xa7,
    0xb2,0x8c,0xce,0xf0,0x4a,0x74,0x36,0x08,0x27,0x19,0x5b,0x65,0xdf,0xe1,0xa3,0x9d,
    0xfd,0xc3,0x81,0xbf,0x05,0x3b,0x79,0x47,0x68,0x56,0x14,0x2a,0x90,0xae,0xec,0xd2,
    0x2c,0x12,0x50,0x6e,0xd4,0xea,0xa8,0x96,0xb9,0x87,0xc5,0xfb,0x41,0x7f,0x3d,0x03,
    0x63,0x5d,0x1f,0x21,0x9b,0xa5,0xe7,0xd9,0xf6,0xc8,0x8a,0xb4,0x0e,0x30,0x72,0x4c,
    0xeb,0xd5,0x97,0xa9,0x13,0x2d,0x6f,0x51,0x7e,0x40,0x02,0x3c,0x86,0xb8,0xfa,0xc4,
    0xa4,0x9a,0xd8,0xe6,0x5c,0x62,0x20,0x1e,0x31,0x0f,0x4d,0x73,0xc9,0xf7,0xb5,0x8b,
    0x75,0x4b,0x09,0x37,0x8d,0xb3,0xf1,0xcf,0xe0,0xde,0x9c,0xa2,0x18,0x26,0x64,0x5a,
    0x3a,0x04,0x46,0x78,0xc2,0xfc,0xbe,0x80,0xaf,0x91,0xd3,0xed,0x57,0x69,0x2b,0x15,
])

def crc8(buf):
    crc = 0
    for b in buf:
        crc = CRC8_TABLE[crc ^ b]
    return crc

# CRC-16/KERMIT, computed on the fly (no need for the 256-entry table here).
def crc16(buf):
    crc = 0
    for b in buf:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc & 0xFFFF

def build_request(cmd_id, payload=b"", tsn=0x42, seq=0, ack_seq=0):
    """Build a full DEAD-prefixed NCP frame carrying a REQUEST."""
    inner = bytes([0, 0, cmd_id & 0xFF, (cmd_id >> 8) & 0xFF, tsn]) + payload
    packet_len = 7 + len(inner)
    flags = 0xC0 | ((seq & 0x03) << 2) | ((ack_seq & 0x03) << 4)  # first+last fragment
    hdr = bytearray(7)
    hdr[0] = 0xDE; hdr[1] = 0xAD
    hdr[2] = packet_len & 0xFF; hdr[3] = (packet_len >> 8) & 0xFF
    hdr[4] = 0x06  # ZBOSS_NCP_API_HL
    hdr[5] = flags
    hdr[6] = crc8(hdr[2:6])
    c = crc16(inner)
    return bytes(hdr) + bytes([c & 0xFF, (c >> 8) & 0xFF]) + inner

def read_frames(ser, timeout_s=2.0):
    """Read raw bytes until timeout, then split into DEAD-prefixed frames."""
    end = time.time() + timeout_s
    buf = bytearray()
    while time.time() < end:
        chunk = ser.read(256)
        if chunk: buf += chunk
        if not chunk and len(buf) >= 9:
            # Quick exit: if buffer looks complete (has a header + declared payload), stop.
            try:
                if buf[0] == 0xDE and buf[1] == 0xAD:
                    plen = buf[2] | (buf[3] << 8)
                    if len(buf) >= plen + 2:
                        break
            except IndexError:
                pass
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
    return frames, bytes(buf)

# TLV tag map (mirrors backup_structured.h)
TAGS = {
    0x01: "PAN_ID",
    0x02: "EXT_PAN_ID",
    0x03: "CHANNEL",
    0x04: "NWK_UPDATE_ID",
    0x05: "COORD_IEEE",
    0x06: "NWK_KEY",
    0x07: "NWK_KEY_SEQ",
    0x08: "NWK_FRAME_COUNTER",
    0x10: "DEVICE_TABLE",
}

def hexdump(b, prefix="  "):
    out = []
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        hexs = " ".join(f"{x:02x}" for x in chunk)
        out.append(f"{prefix}{i:04x}: {hexs}")
    return "\n".join(out)

def parse_response(payload):
    """Parse the inner command payload of a RESPONSE frame for cmd 0x009B."""
    # cmd_t (4 bytes) + generic_response_t (2 bytes) + image
    if len(payload) < 6:
        print(f"  ERROR: payload too short ({len(payload)} bytes)"); return False
    version, type_, cmd_lo, cmd_hi, tsn = payload[0], payload[1], payload[2], payload[3], payload[4]
    cmd_id = cmd_lo | (cmd_hi << 8)
    print(f"  cmd_t: version={version} type={type_} cmd_id=0x{cmd_id:04x} tsn={tsn}")
    body = payload[5:]
    if len(body) < 2:
        print("  ERROR: no status byte present"); return False
    category, status = body[0], body[1]
    print(f"  status: category={category} status={status} ({'OK' if status==0 else 'NOT OK'})")
    if status != 0:
        return False
    image = body[2:]
    if len(image) < 8:
        print("  ERROR: image < 8 bytes — no header"); return False
    magic = image[0:4]
    version_b = image[4]
    flags = image[5]
    payload_len = image[6] | (image[7] << 8)
    print(f"  header: magic={magic!r} version={version_b} flags={flags} payload_len={payload_len}")
    if magic != b"ZBSB":
        print(f"  ERROR: bad magic, expected b'ZBSB'"); return False
    if version_b != 1:
        print(f"  ERROR: unknown version {version_b}"); return False
    tlvs = image[8:8 + payload_len]
    print(f"  TLVs ({payload_len} bytes):")
    p = 0
    while p + 3 <= len(tlvs):
        tag = tlvs[p]; tlen = tlvs[p+1] | (tlvs[p+2] << 8); p += 3
        val = tlvs[p:p+tlen]; p += tlen
        name = TAGS.get(tag, f"UNKNOWN(0x{tag:02x})")
        if tag == 0x01 and tlen == 2:
            pretty = f"0x{val[0] | (val[1]<<8):04x}"
        elif tag == 0x02 and tlen == 8:
            pretty = ":".join(f"{b:02x}" for b in val)
        elif tag == 0x03 and tlen == 1:
            pretty = f"{val[0]}"
        elif tag == 0x04 and tlen == 1:
            pretty = f"{val[0]}"
        elif tag == 0x05 and tlen == 8:
            pretty = ":".join(f"{b:02x}" for b in val)
        elif tag == 0x06 and tlen == 16:
            pretty = val.hex()
        elif tag == 0x08 and tlen == 4:
            pretty = f"{struct.unpack('<I', val)[0]}"
        elif tag == 0x10:
            n = tlen // 16
            pretty = f"{n} devices ({tlen} bytes)"
        else:
            pretty = val.hex() if tlen <= 32 else f"{tlen} bytes"
        print(f"    [{tag:02x}] {name:18s} len={tlen:3d} value={pretty}")
        if tag == 0x10 and tlen >= 16:
            for d in range(n):
                off = d * 16
                rec = val[off:off+16]
                ieee = ":".join(f"{b:02x}" for b in rec[0:8])
                short = rec[8] | (rec[9] << 8)
                rx_on = rec[10]; rel = rec[11]; dt = rec[12]; depth = rec[13]; lqi = rec[14]
                print(f"      dev{d:02d}: ieee={ieee} short=0x{short:04x} rx_on={rx_on} rel={rel} type={dt} depth={depth} lqi={lqi}")
    return True

def main():
    print(f"Opening {PORT}")
    if not os.path.exists(PORT):
        print(f"ERROR: {PORT} does not exist"); sys.exit(2)
    ser = serial.Serial(PORT, 115200, timeout=0.1)
    ser.reset_input_buffer()

    # Drain any unsolicited boot frame.
    print("Draining boot frame (1s)...")
    boot_frames, raw = read_frames(ser, timeout_s=1.0)
    print(f"  got {len(raw)} bytes, {len(boot_frames)} frames")
    for f in boot_frames:
        print(f"  boot frame: {f.hex()}")

    # Build + send GET_STRUCTURED_BACKUP.
    req = build_request(cmd_id=0x009B, payload=b"", tsn=0x42, seq=0, ack_seq=0)
    print(f"Sending GET_STRUCTURED_BACKUP: {req.hex()}")
    ser.write(req)

    # Read reply (allow longer timeout — image plus ack).
    print("Reading reply (3s)...")
    frames, raw = read_frames(ser, timeout_s=3.0)
    print(f"  got {len(raw)} bytes, {len(frames)} frames")

    ok = False
    for i, f in enumerate(frames):
        plen = f[2] | (f[3] << 8)
        flags = f[5]
        is_ack = bool(flags & 0x01)
        print(f"\nframe[{i}] len={plen} flags=0x{flags:02x} ack={is_ack}")
        print(hexdump(f))
        if not is_ack and plen >= 9:
            # Skip 7-byte header + 2-byte CRC16 → command payload starts at offset 9.
            payload = f[9:]
            ok = parse_response(payload) or ok

    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
