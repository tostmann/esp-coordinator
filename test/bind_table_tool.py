#!/usr/bin/env python3
# bind_table_tool.py — dump (and optionally clean) a Zigbee device's binding
# table through an esp-coordinator NCP stick.
#
# Background: firmware before v1.5.73 reported the coordinator's IEEE address
# byte-reversed (issue #6). Hosts stored that phantom address and wrote it into
# device binding tables as the bind destination. Devices with a small binding
# table can end up permanently full of phantom entries, so every later
# configure-bind fails with ZDO status 0x8C (TABLE_FULL) — see issue #7.
# This tool sends ZDO Mgmt_Bind_req to the device, classifies every entry
# against the coordinator's true IEEE, and can unbind the phantom entries.
#
# Usage (STOP Zigbee2MQTT / any other host first — the stick has one NCP link):
#   python3 bind_table_tool.py PORT NWK_ADDR [--unbind-phantoms]
#     PORT      serial port (/dev/ttyACM0, /dev/serial/by-id/...) or tcp://IP:6638
#     NWK_ADDR  the device's network address as shown by Z2M, e.g. 0x4711
# The device must be awake/mains-powered (a sleepy device needs a button press).
# Requires python3-serial for serial ports; TCP mode has no dependencies.
#
# Read-only by default. --unbind-phantoms sends one ZDO Unbind_req per phantom
# entry and re-dumps the table to verify. Exit codes: 0 ok, 1 link/comm error,
# 2 ZDO error from the device.

import sys, time, argparse, socket

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

def build_request(cmd_id, payload=b"", tsn=0x42):
    inner = bytes([0, 0, cmd_id & 0xFF, (cmd_id >> 8) & 0xFF, tsn]) + payload
    packet_len = 7 + len(inner)
    hdr = bytearray(7)
    hdr[0] = 0xDE; hdr[1] = 0xAD
    hdr[2] = packet_len & 0xFF; hdr[3] = (packet_len >> 8) & 0xFF
    hdr[4] = 0x06; hdr[5] = 0xC0; hdr[6] = crc8(hdr[2:6])
    c = crc16(inner)
    return bytes(hdr) + bytes([c & 0xFF, (c >> 8) & 0xFF]) + inner

class TcpPort:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.settimeout(0.1)
    def read(self, n):
        try:
            return self.s.recv(n)
        except socket.timeout:
            return b""
    def write(self, b):
        self.s.sendall(b)
    def reset_input_buffer(self):
        while True:
            try:
                if not self.s.recv(4096):
                    break
            except socket.timeout:
                break
    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

def open_port(spec):
    if spec.startswith("tcp://"):
        host, _, port = spec[6:].partition(":")
        p = TcpPort(host, int(port or 6638))
        drain(p, 3.0)
        return p
    import serial
    p = serial.Serial()
    p.port = spec
    p.baudrate = 115200
    p.timeout = 0.1
    p.write_timeout = 2.0
    p.dtr = False; p.rts = False
    p.open()
    time.sleep(0.3)
    drain(p, 3.0)   # boot / boot-ready frames after a possible open-pulse reset
    return p

def drain(port, secs):
    end = time.time() + secs
    while time.time() < end:
        port.read(4096)

def txn(port, cmd_id, payload, tsn, timeout_s=6.0):
    """Send one request, return (zdo_or_generic_category, status, body) of the
    first RESPONSE frame for cmd_id, or None on timeout."""
    port.reset_input_buffer()
    port.write(build_request(cmd_id, payload, tsn=tsn))
    end = time.time() + timeout_s
    buf = bytearray()
    while time.time() < end:
        chunk = port.read(512)
        if chunk:
            buf += chunk
        i = 0
        while i + 9 <= len(buf):
            if buf[i] != 0xDE or buf[i + 1] != 0xAD:
                i += 1
                continue
            plen = buf[i + 2] | (buf[i + 3] << 8)
            flen = 2 + plen
            if i + flen > len(buf):
                break
            f = bytes(buf[i:i + flen]); i += flen
            if f[5] & 0x01:            # ACK frame
                continue
            pl = f[9:]
            if len(pl) < 7 or (pl[2] | (pl[3] << 8)) != cmd_id:
                continue
            return pl[5], pl[6], pl[7:]
        buf = buf[i:]
    return None

def txn_retry(port, cmd_id, payload, tsn_base, tries=3, timeout_s=6.0):
    for attempt in range(tries):
        r = txn(port, cmd_id, payload, (tsn_base + attempt) & 0xFF, timeout_s)
        if r is not None:
            return r
    return None

def hexle(b):
    return ":".join(f"{x:02x}" for x in b)

def ieee_disp(le_bytes):
    """LE wire bytes -> the 0x... display form Z2M shows (MSB first)."""
    return "0x" + "".join(f"{x:02x}" for x in reversed(le_bytes))

def parse_records(body):
    """body = [entries, start_index, count] + records (after cat+status)."""
    entries, start, count = body[0], body[1], body[2]
    recs = []
    p = 3
    for _ in range(count):
        if p + 12 > len(body):
            break
        src = bytes(body[p:p + 8]); p += 8
        src_ep = body[p]; p += 1
        cluster = body[p] | (body[p + 1] << 8); p += 2
        mode = body[p]; p += 1
        dst = None; dst_short = None; dst_ep = None
        if mode == 0x01:
            dst_short = body[p] | (body[p + 1] << 8); p += 2
        elif mode == 0x03:
            dst = bytes(body[p:p + 8]); p += 8
            dst_ep = body[p]; p += 1
        recs.append(dict(src=src, src_ep=src_ep, cluster=cluster,
                         mode=mode, dst=dst, dst_short=dst_short, dst_ep=dst_ep))
    return entries, start, count, recs

def dump_table(port, nwk):
    all_recs = []
    start = 0
    entries = None
    tsn = 0x40
    while True:
        r = txn_retry(port, 0x020F, nwk.to_bytes(2, "little") + bytes([start]), tsn)
        tsn = (tsn + 4) & 0xFF
        if r is None:
            print("!! no response to Mgmt_Bind_req — is the device awake/on the network, "
                  "and is Z2M really stopped?")
            sys.exit(1)
        cat, status, body = r
        if status != 0:
            print(f"!! Mgmt_Bind_req rejected: ZDO status 0x{status:02x}")
            sys.exit(2)
        entries, got_start, count, recs = parse_records(body)
        all_recs += recs
        if count == 0 or got_start + count >= entries:
            break
        start = got_start + count
    return entries, all_recs

def classify(rec, ieee_true, ieee_phantom):
    if rec["mode"] != 0x03 or rec["dst"] is None:
        return "group/other-mode"
    if rec["dst"] == ieee_true:
        return "coordinator (correct)"
    if rec["dst"] == ieee_phantom:
        return "PHANTOM (byte-reversed coordinator)"
    return "other device"

def main():
    ap = argparse.ArgumentParser(description="Dump/clean a device binding table via an esp-coordinator stick")
    ap.add_argument("port", help="/dev/ttyACM0 | /dev/serial/by-id/... | tcp://IP:6638")
    ap.add_argument("nwk", help="device network address as shown by Z2M, e.g. 0x4711")
    ap.add_argument("--unbind-phantoms", action="store_true",
                    help="send Unbind_req for every phantom entry, then re-dump to verify")
    args = ap.parse_args()
    nwk = int(args.nwk, 0)

    port = open_port(args.port)

    # -- coordinator identity ------------------------------------------------
    r = txn_retry(port, 0x0001, b"", 0x01, timeout_s=3.0)
    if r is None:
        print("!! coordinator does not answer (GET_MODULE_VERSION) — wrong port, "
              "or another host (Z2M?) is still holding the link")
        sys.exit(1)
    ver_raw = int.from_bytes(r[2][0:4], "little") if (r[1] == 0 and len(r[2]) >= 4) else 0
    fw = ((ver_raw >> 24) & 0xFF, (ver_raw >> 16) & 0xFF, ver_raw & 0xFFFF)
    r = txn_retry(port, 0x000B, b"\x00", 0x05, timeout_s=3.0)
    if r is None or r[1] != 0 or len(r[2]) < 9:
        print("!! GET_LOCAL_IEEE_ADDR failed — cannot classify entries")
        sys.exit(1)
    reported = bytes(r[2][1:9])          # LE wire order on >= v1.5.73
    if fw >= (1, 5, 73):
        ieee_true = reported
    else:
        # pre-fix firmware reports the IEEE byte-reversed (issue #6)
        ieee_true = bytes(reversed(reported))
        print(f"NOTE: firmware {fw[0]}.{fw[1]}.{fw[2]} predates the issue-#6 fix — "
              "its reported IEEE is byte-reversed; corrected for classification.")
    ieee_phantom = bytes(reversed(ieee_true))
    print(f"coordinator fw {fw[0]}.{fw[1]}.{fw[2]}  IEEE {ieee_disp(ieee_true)}  "
          f"(phantom would be {ieee_disp(ieee_phantom)})")

    # -- dump ----------------------------------------------------------------
    entries, recs = dump_table(port, nwk)
    print(f"\ndevice 0x{nwk:04x}: binding table reports {entries} entries, "
          f"{len(recs)} parsed:")
    phantoms = []
    for i, rec in enumerate(recs):
        cls = classify(rec, ieee_true, ieee_phantom)
        if cls.startswith("PHANTOM"):
            phantoms.append(rec)
        dst = (f"{ieee_disp(rec['dst'])}/{rec['dst_ep']}" if rec["mode"] == 0x03
               else f"group 0x{rec['dst_short']:04x}" if rec["mode"] == 0x01
               else f"mode 0x{rec['mode']:02x}")
        print(f"  [{i:2d}] src {ieee_disp(rec['src'])}/{rec['src_ep']}  "
              f"cluster 0x{rec['cluster']:04x}  ->  {dst}   {cls}")
    print(f"\nsummary: {len(recs)} entries, {len(phantoms)} phantom, "
          f"{sum(1 for x in recs if classify(x, ieee_true, ieee_phantom) == 'coordinator (correct)')} "
          f"correct-coordinator, rest other")

    # -- optional cleanup ----------------------------------------------------
    if args.unbind_phantoms and phantoms:
        print(f"\nunbinding {len(phantoms)} phantom entries ...")
        tsn = 0x80
        failed = 0
        for rec in phantoms:
            payload = (nwk.to_bytes(2, "little") + rec["src"] +
                       bytes([rec["src_ep"]]) + rec["cluster"].to_bytes(2, "little") +
                       bytes([0x03]) + rec["dst"] + bytes([rec["dst_ep"] or 0]))
            r = txn_retry(port, 0x0209, payload, tsn)
            tsn = (tsn + 4) & 0xFF
            if r is None or r[1] != 0:
                failed += 1
                print(f"  unbind cluster 0x{rec['cluster']:04x} -> "
                      f"{'no response' if r is None else 'ZDO status 0x%02x' % r[1]}")
            else:
                print(f"  unbind cluster 0x{rec['cluster']:04x} -> OK")
        entries, recs = dump_table(port, nwk)
        left = [x for x in recs if classify(x, ieee_true, ieee_phantom).startswith("PHANTOM")]
        print(f"\nafter cleanup: {entries} entries, {len(left)} phantom left"
              + (f" ({failed} unbind failures)" if failed else ""))
        sys.exit(0 if not left and not failed else 2)
    elif args.unbind_phantoms:
        print("\nnothing to unbind — no phantom entries found.")

if __name__ == "__main__":
    main()
