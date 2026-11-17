#!/usr/bin/env python3
"""Read-only probe of a ZBOSS NCP over TCP: version, joined, channel, PAN ids.

usage: ncp_probe.py <host> [port] [--reset]
Framing (CRC tables, build_request) from tostmann/esp-coordinator test/
getset_ext_pan_check.py (MIT). Without --reset it sends only GET_* requests.
Disconnect Zigbee2MQTT/ZHA first: the NCP serves one host at a time.

--reset sends NCP_RESET with option 0: a plain restart that keeps the
network and clears the boot guard (the way out of safe mode, where every
other command answers GENERIC_BLOCKED). Options 1/2 (erase NVRAM / factory
reset) are deliberately not offered.
"""
import socket
import sys
import time

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

def read_frames(sock, timeout_s=2.0):
    end = time.time() + timeout_s
    buf = bytearray()
    sock.settimeout(0.2)
    while time.time() < end:
        try:
            chunk = sock.recv(512)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
    frames, i = [], 0
    while i + 7 <= len(buf):
        if buf[i] != 0xDE or buf[i + 1] != 0xAD:
            i += 1
            continue
        plen = buf[i + 2] | (buf[i + 3] << 8)
        if i + 2 + plen > len(buf):
            break
        frames.append(bytes(buf[i:i + 2 + plen]))
        i += 2 + plen
    return frames


_seq = 0


def send_ack(sock, frame):
    """ACK a data frame from the NCP (else it keeps retransmitting it and
    holds back the next response)."""
    pkt_seq = (frame[5] >> 2) & 0x03
    hdr = bytearray([0xDE, 0xAD, 0x05, 0x00, 0x06, 0x01 | (pkt_seq << 4), 0])
    hdr[6] = crc8(hdr[2:6])
    sock.sendall(bytes(hdr))


def txn(sock, cmd_id, tsn):
    global _seq
    _seq = (_seq % 3) + 1   # fresh packet_seq each request (NCP drops duplicates)
    sock.sendall(build_request(cmd_id, tsn=tsn, seq=_seq))
    for f in read_frames(sock, 2.0):
        if f[5] & 0x01 or len(f) < 14:   # ACK frame / too short
            continue
        send_ack(sock, f)
        pl = f[9:]
        if (pl[2] | (pl[3] << 8)) != cmd_id:
            continue
        body = pl[5:]
        return body[0], body[1], body[2:]
    return None


def main():
    global _seq
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    reset = "--reset" in sys.argv
    host = args[0]
    port = int(args[1]) if len(args) > 1 else 6638
    sock = socket.create_connection((host, port), timeout=5)
    boot = read_frames(sock, 1.5)
    for f in boot:
        if not f[5] & 0x01:
            send_ack(sock, f)
    print(f"connected, {len(boot)} boot frame(s)")
    if reset:
        # NCP_RESET option 0: plain restart, network kept, boot guard cleared.
        _seq = 1
        sock.sendall(build_request(0x0002, payload=b"\x00", tsn=0x30, seq=_seq))
        got = [f for f in read_frames(sock, 2.0) if not f[5] & 0x01]
        print("NCP_RESET sent (option 0: restart, network kept);",
              "answered" if got else "no answer (restarting)")
        sock.close()
        return
    checks = [
        ("module version", 0x0001, lambda b: "fw %d.%d.%d.%d stack %d.%d.%d.%d proto %d.%d.%d.%d" % (*b[0:4][::-1], *b[4:8][::-1], *b[8:12][::-1])),
        ("joined", 0x0014, lambda b: str(bool(b[0]))),
        ("channel", 0x0008, lambda b: f"page {b[0]} channel {b[1]}"),
        ("PAN id", 0x0009, lambda b: f"0x{b[0] | (b[1] << 8):04X}"),
        ("extended PAN id", 0x0023, lambda b: ":".join(f"{x:02x}" for x in b[:8][::-1])),
    ]
    ok = True
    for i, (name, cmd, fmt) in enumerate(checks):
        r = txn(sock, cmd, 0x10 + i)
        if r is None:
            print(f"{name:16s} no response")
            ok = False
        elif r[1] != 0:
            print(f"{name:16s} status {r[0]}/{r[1]}")
            ok = False
        else:
            print(f"{name:16s} {fmt(r[2])}")
    sock.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
