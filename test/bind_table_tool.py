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
#   python3 bind_table_tool.py PORT NWK_ADDR [--unbind-phantoms | --test-bind | --purge-phantom-blind]
#     PORT      serial port (/dev/ttyACM0, /dev/serial/by-id/...) or tcp://IP:6638
#     NWK_ADDR  the device's network address as shown by Z2M, e.g. 0x4711
# The device must be awake/mains-powered (a sleepy device needs a button press).
# Requires python3-serial for serial ports; TCP mode has no dependencies.
#
# Read-only by default. --unbind-phantoms sends one ZDO Unbind_req per phantom
# entry and re-dumps the table to verify. Exit codes: 0 ok, 1 link/comm error,
# 2 ZDO error from the device.
#
# --test-bind: differential bind probe (issue #7). Sends three Bind_reqs for
# (--src-ep, --cluster) with three different destination IEEEs:
#   1. the coordinator's real IEEE   (what a >=v1.5.73 host writes)
#   2. a neutral test IEEE           (locally administered, owned by nobody)
#   3. the phantom IEEE              (byte-reversed real - what a <=v1.5.70
#                                     host wrote, issue #6)
# and records the ZDO status of each. Every successful bind is unbound again
# immediately, so the probe leaves no state behind; a final table dump
# verifies that. This discriminates "device rejects binds depending on the
# destination IEEE value" from "device rejects all binds right now".
# A mid-probe dump (between the first successful bind and its unbind) checks
# whether the device's Mgmt_Bind reporting actually reflects the table.
# Target 0x0000 (the coordinator itself) skips the mid/final dumps: a
# self-addressed bind/unbind wedges subsequent self-addressed Mgmt_Bind in the
# stack until reboot (bench-verified on v1.5.73; remote targets unaffected).
#
# --purge-phantom-blind: remove phantom entries WITHOUT relying on Mgmt_Bind
# (some devices provably under-report their binding table, so --unbind-phantoms
# never sees them). Sends one ZDO Unbind_req per (src_ep, cluster) combination
# from a fixed list (default: the binds Z2M's configure writes for the Aqara
# WS-K07E — override with --purge-list "ep:cluster,ep:cluster,..."), each with
# dst = the phantom IEEE. Unbind doubles as forensics: SUCCESS = the entry
# existed and is now removed; NO_ENTRY (0x88) = it was never there. Afterwards
# a verify bind with the neutral test IEEE (never a real entry, so removing it
# again cannot touch a live bind) checks whether the table accepts new entries
# again. Restart Z2M and re-run Configure after a successful purge.

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

def dump_table(port, nwk, soft=False):
    all_recs = []
    start = 0
    entries = None
    tsn = 0x40
    while True:
        r = txn_retry(port, 0x020F, nwk.to_bytes(2, "little") + bytes([start]), tsn)
        tsn = (tsn + 4) & 0xFF
        if r is None:
            if soft:
                return None, []
            print("!! no response to Mgmt_Bind_req — is the device awake/on the network, "
                  "and is Z2M really stopped?")
            sys.exit(1)
        cat, status, body = r
        if status != 0:
            if soft:
                return None, []
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

# ---------------------------------------------------------------------------
# --test-bind: differential bind probe (issue #7)
# ---------------------------------------------------------------------------

ZDO_STATUS_NAMES = {
    0x00: "SUCCESS", 0x80: "INV_REQUESTTYPE", 0x84: "NOT_SUPPORTED",
    0x85: "TIMEOUT", 0x88: "NO_ENTRY", 0x8C: "TABLE_FULL",
    0x8D: "NOT_AUTHORIZED",
}

def status_str(r):
    if r is None:
        return "NO RESPONSE (timeout)"
    cat, status, _ = r
    name = ZDO_STATUS_NAMES.get(status, "?")
    return f"status 0x{status:02x} {name} (category {cat})"

def resolve_ieee(port, nwk):
    """Device IEEE via ZDO_IEEE_ADDR_REQ (0x0202); returns LE wire bytes."""
    payload = (nwk.to_bytes(2, "little") + nwk.to_bytes(2, "little") +
               bytes([0x00, 0x00]))          # single-device request, index 0
    r = txn_retry(port, 0x0202, payload, 0x30)
    if r is None or r[1] != 0 or len(r[2]) < 8:
        return None
    return bytes(r[2][0:8])

def bind_payload(nwk, src_ieee, src_ep, cluster, dst_ieee, dst_ep):
    """Shared wire layout of ZDO_BIND_REQ (0x0208) / ZDO_UNBIND_REQ (0x0209):
    target(2) srcIeee(8) srcEP(1) cluster(2) addrMode(1)=0x03 dstIeee(8) dstEP(1)."""
    return (nwk.to_bytes(2, "little") + src_ieee + bytes([src_ep]) +
            cluster.to_bytes(2, "little") + bytes([0x03]) + dst_ieee +
            bytes([dst_ep]))

# locally administered EUI-64 (U/L bit set) - guaranteed to belong to nobody
NEUTRAL_IEEE_LE = bytes(reversed(bytes.fromhex("02deadbeef001234")))

# Default --purge-phantom-blind combination list: every bind Z2M's configure
# writes for the Aqara WS-K07E (zigbee-herdsman-converters lumi.ts "WS-K07E":
# bindCluster manuSpecificLumi ep1+ep4, bindCluster genOnOff ep1, plus
# lumiOnOff->onOff() which binds genOnOff on every endpoint carrying it), and
# defensively the electricityMeter clusters older converter releases may have
# bound. Unbinding a combination that never existed just answers NO_ENTRY.
PURGE_DEFAULT = "1:0xfcc0,4:0xfcc0,1:0x0006,4:0x0006,1:0x0b04,1:0x0702"

def run_bind_probe(port, nwk, src_ieee, src_ep, cluster, dst_ep,
                   ieee_true, ieee_phantom):
    candidates = [
        ("real coordinator IEEE", ieee_true),
        ("neutral test IEEE",     NEUTRAL_IEEE_LE),
        ("phantom IEEE (issue#6)", ieee_phantom),
    ]
    print(f"\nbind probe: src {ieee_disp(src_ieee)}/{src_ep}  "
          f"cluster 0x{cluster:04x}  dst_ep {dst_ep}")
    results = []
    leftovers = []
    # Visibility check: after the FIRST successful bind (before its unbind) the
    # table must show the fresh entry — if it doesn't, the device's Mgmt_Bind
    # reporting is proven unreliable. None = check not run. Skipped for the
    # coordinator itself (nwk 0x0000): a self-addressed bind/unbind wedges the
    # stack's subsequent self-addressed Mgmt_Bind until reboot (bench-verified
    # on v1.5.73), so self-probes must not dump mid-sequence.
    mid_visible = None
    tsn = 0x60
    for label, dst in candidates:
        pl = bind_payload(nwk, src_ieee, src_ep, cluster, dst, dst_ep)
        r = txn_retry(port, 0x0208, pl, tsn, timeout_s=8.0)
        tsn = (tsn + 4) & 0xFF
        bound = r is not None and r[1] == 0
        line = f"  bind   -> {ieee_disp(dst)}/{dst_ep} ({label}): {status_str(r)}"
        results.append((label, r))
        if bound:
            if mid_visible is None and nwk != 0x0000:
                _, mrecs = dump_table(port, nwk, soft=True)
                mid_visible = any(rec["src_ep"] == src_ep and
                                  rec["cluster"] == cluster and
                                  rec["dst"] == dst for rec in mrecs)
            time.sleep(0.3)
            ru = txn_retry(port, 0x0209, pl, tsn, timeout_s=8.0)
            tsn = (tsn + 4) & 0xFF
            if ru is None or ru[1] != 0:
                leftovers.append((label, dst))
                line += f"\n  unbind -> FAILED: {status_str(ru)}  ** ENTRY LEFT BEHIND **"
            else:
                line += "  [unbound again, clean]"
        print(line)
        time.sleep(0.3)
    return results, leftovers, mid_visible

def run_phantom_purge(port, nwk, src_ieee, combos, dst_ep, ieee_phantom):
    """Blind-unbind (src_ep, cluster) x phantom-IEEE without trusting Mgmt_Bind.
    Returns (removed, absent, errors, verify_ok)."""
    print(f"\nblind phantom purge: src {ieee_disp(src_ieee)}  "
          f"dst {ieee_disp(ieee_phantom)}/{dst_ep} (phantom)")
    removed, absent, errors = [], [], []
    tsn = 0xA0
    for ep, cluster in combos:
        pl = bind_payload(nwk, src_ieee, ep, cluster, ieee_phantom, dst_ep)
        r = txn_retry(port, 0x0209, pl, tsn, timeout_s=8.0)
        tsn = (tsn + 4) & 0xFF
        if r is not None and r[1] == 0x00:
            removed.append((ep, cluster))
            verdict = "REMOVED (entry existed)"
        elif r is not None and r[1] == 0x88:
            absent.append((ep, cluster))
            verdict = "not present (NO_ENTRY)"
        else:
            errors.append((ep, cluster))
            verdict = status_str(r)
        print(f"  unbind ep{ep} cluster 0x{cluster:04x} -> {verdict}")
        time.sleep(0.3)

    # verify: can the table take a NEW entry now? Uses the neutral IEEE (never
    # a real destination), so the immediate unbind cannot remove a live bind.
    vep, vcluster = combos[0]
    pl = bind_payload(nwk, src_ieee, vep, vcluster, NEUTRAL_IEEE_LE, dst_ep)
    r = txn_retry(port, 0x0208, pl, tsn, timeout_s=8.0)
    tsn = (tsn + 4) & 0xFF
    verify_ok = r is not None and r[1] == 0x00
    print(f"\nverify bind (neutral IEEE, ep{vep}/0x{vcluster:04x}): {status_str(r)}")
    if verify_ok:
        time.sleep(0.3)
        ru = txn_retry(port, 0x0209, pl, tsn, timeout_s=8.0)
        if ru is None or ru[1] != 0:
            print(f"  !! WARNING: verify unbind failed ({status_str(ru)}) — "
                  f"neutral entry {ieee_disp(NEUTRAL_IEEE_LE)} left behind, re-run to remove")
        else:
            print("  verify entry unbound again, clean — table accepts new binds now")
    else:
        print("  table still refuses a new entry — purge did not free a slot "
              "(different combinations may be occupying it; try --purge-list)")

    print(f"\npurge summary: {len(removed)} removed, {len(absent)} not present, "
          f"{len(errors)} errors; new-bind verify: {'PASS' if verify_ok else 'FAIL'}")
    if removed:
        print("removed entries:")
        for ep, cluster in removed:
            print(f"  ep{ep} cluster 0x{cluster:04x} -> was bound to the phantom IEEE")
    if verify_ok:
        print("\nnext step: restart Z2M and run Configure on the device.")
    return removed, absent, errors, verify_ok

def parse_purge_list(spec):
    combos = []
    for item in spec.split(","):
        ep, _, cluster = item.strip().partition(":")
        combos.append((int(ep, 0), int(cluster, 0)))
    return combos

def main():
    ap = argparse.ArgumentParser(description="Dump/clean a device binding table via an esp-coordinator stick")
    ap.add_argument("port", help="/dev/ttyACM0 | /dev/serial/by-id/... | tcp://IP:6638")
    ap.add_argument("nwk", help="device network address as shown by Z2M, e.g. 0x4711")
    ap.add_argument("--unbind-phantoms", action="store_true",
                    help="send Unbind_req for every phantom entry, then re-dump to verify")
    ap.add_argument("--test-bind", action="store_true",
                    help="differential bind probe: bind to real/neutral/phantom "
                         "IEEE, record the three ZDO statuses, unbind again")
    ap.add_argument("--purge-phantom-blind", action="store_true",
                    help="blindly unbind known phantom binds without trusting "
                         "Mgmt_Bind (for devices that under-report their table); "
                         "SUCCESS = entry existed, NO_ENTRY = it did not")
    ap.add_argument("--purge-list", default=PURGE_DEFAULT,
                    help="ep:cluster combinations for --purge-phantom-blind "
                         f"(default {PURGE_DEFAULT})")
    ap.add_argument("--cluster", default="0xfcc0",
                    help="cluster for --test-bind (default 0xfcc0 manuSpecificLumi)")
    ap.add_argument("--src-ep", default="1", help="device source endpoint (default 1)")
    ap.add_argument("--dst-ep", default="1", help="destination endpoint (default 1)")
    ap.add_argument("--src-ieee",
                    help="device IEEE override as 0x... (default: resolved via "
                         "ZDO_IEEE_ADDR_REQ)")
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

    # -- shared src-IEEE resolve for the active-probe modes ------------------
    src_ieee = None
    if args.test_bind or args.purge_phantom_blind:
        if args.src_ieee:
            src_ieee = bytes(reversed(bytes.fromhex(args.src_ieee.replace("0x", ""))))
        else:
            src_ieee = resolve_ieee(port, nwk)
            if src_ieee is None:
                print("\n!! could not resolve the device IEEE (ZDO_IEEE_ADDR_REQ) — "
                      "pass it explicitly via --src-ieee 0x...")
                sys.exit(1)

    # -- optional differential bind probe ------------------------------------
    exit_code = 0
    if args.test_bind:
        pre_entries = entries
        results, leftovers, mid_visible = run_bind_probe(
            port, nwk, src_ieee,
            int(args.src_ep, 0), int(args.cluster, 0), int(args.dst_ep, 0),
            ieee_true, ieee_phantom)

        if nwk != 0x0000:
            entries2, recs2 = dump_table(port, nwk, soft=True)
            print(f"\nafter probe: table reports "
                  f"{'?' if entries2 is None else entries2} entries "
                  f"(was {pre_entries} before)")
        else:
            print("\n(final table dump skipped for the coordinator itself — "
                  "self-addressed Mgmt_Bind wedges after a self bind/unbind "
                  "until reboot)")
        if mid_visible is True:
            print("mid-probe check: fresh bind entry WAS visible via Mgmt_Bind — "
                  "table reporting looks reliable on this device.")
        elif mid_visible is False:
            print("mid-probe check: fresh bind entry NOT visible via Mgmt_Bind — "
                  "this device's Mgmt_Bind reporting does not reflect its real "
                  "binding table (known device-firmware quirk); 0-entry dumps "
                  "from it prove nothing.")
        if leftovers:
            print("!! WARNING: probe entries left behind (unbind failed) — "
                  "re-run, or remove manually:")
            for label, dst in leftovers:
                print(f"     {label}: dst {ieee_disp(dst)}")
        print("\nprobe summary:")
        for label, r in results:
            print(f"  {label:24s} {status_str(r)}")
        if leftovers:
            exit_code = 2

    # -- optional blind phantom purge ----------------------------------------
    if args.purge_phantom_blind:
        if nwk == 0x0000:
            print("\n!! --purge-phantom-blind refused for the coordinator itself "
                  "(0x0000): self-addressed ZDO bind/unbind wedges the stack "
                  "(see header notes)")
            sys.exit(1)
        combos = parse_purge_list(args.purge_list)
        if not combos:
            print("\n!! --purge-list is empty")
            sys.exit(1)
        _, _, perrors, verify_ok = run_phantom_purge(
            port, nwk, src_ieee, combos, int(args.dst_ep, 0), ieee_phantom)
        if perrors or not verify_ok:
            exit_code = max(exit_code, 2)

    sys.exit(exit_code)

if __name__ == "__main__":
    main()
