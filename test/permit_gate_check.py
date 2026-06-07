#!/usr/bin/env python3
# BOOT-1 on-air validation: after a plain boot the coordinator must NOT accept
# joins (no self-initiated permit-join window); after a host NWK_PERMIT_JOINING
# it must. Requires a factory-new steering probe (e.g. HA_on_off_light) in
# range that continuously retries network steering.
#
# Phase 1 (closed): open port (= device reset = the boot under test), drain
#   boot frames, then listen passively for PHASE1_S seconds. Any unsolicited
#   join indication (ZDO_DEV_UPDATE_IND 0x0215 / ZDO_DEV_ANNCE_IND 0x020c /
#   ZDO_DEV_AUTHORIZED_IND 0x0214) = FAIL (probe got in without permit).
# Phase 2 (open): send NWK_PERMIT_JOINING(0x0404, duration=180), expect OK,
#   then listen up to PHASE2_S seconds. A join indication = PASS (probe joined
#   once the host opened the network).
#
# Framing byte-identical to dup_drop_check.py. Usage:
#   python3 permit_gate_check.py /dev/ttyACM4
import sys, time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM4"
PHASE1_S = 220
PHASE2_S = 180

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

NWK_PERMIT_JOINING = 0x0404
JOIN_INDICATIONS = {0x020c: "ZDO_DEV_ANNCE_IND", 0x0214: "ZDO_DEV_AUTHORIZED_IND", 0x0215: "ZDO_DEV_UPDATE_IND"}

def build_request(cmd_id, payload=b"", tsn=0x42, seq=0):
    inner = bytes([0, 0, cmd_id & 0xFF, (cmd_id >> 8) & 0xFF, tsn]) + payload
    packet_len = 7 + len(inner)
    flags = 0xC0 | ((seq & 0x03) << 2)
    hdr = bytearray(7)
    hdr[0] = 0xDE; hdr[1] = 0xAD
    hdr[2] = packet_len & 0xFF; hdr[3] = (packet_len >> 8) & 0xFF
    hdr[4] = 0x06
    hdr[5] = flags
    hdr[6] = crc8(hdr[2:6])
    c = crc16(inner)
    return bytes(hdr) + bytes([c & 0xFF, (c >> 8) & 0xFF]) + inner

class FrameReader:
    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()
    def poll(self, seconds):
        """Read for `seconds`, return list of complete frames."""
        end = time.time() + seconds
        frames = []
        while time.time() < end:
            chunk = self.ser.read(256)
            if chunk:
                self.buf += chunk
            while True:
                i = self.buf.find(b"\xde\xad")
                if i < 0:
                    if len(self.buf) > 1:
                        del self.buf[:-1]
                    break
                if i > 0:
                    del self.buf[:i]
                if len(self.buf) < 4:
                    break
                plen = self.buf[2] | (self.buf[3] << 8)
                if len(self.buf) < 2 + plen:
                    break
                frames.append(bytes(self.buf[:2 + plen]))
                del self.buf[:2 + plen]
        return frames

def classify_indications(frames):
    """Return list of (cmd_id, name) for INDICATION-type payloads."""
    out = []
    for f in frames:
        if len(f) < 9 + 5:
            continue
        if f[5] & 0x01:           # ACK frame
            continue
        pl = f[9:]
        ftype = pl[1]             # 0=req 1=rsp 2=indication
        cmd = pl[2] | (pl[3] << 8)
        if ftype == 2:
            out.append((cmd, JOIN_INDICATIONS.get(cmd, f"IND_0x{cmd:04x}")))
    return out

def main():
    print(f"== BOOT-1 permit-gate on-air check on {PORT} ==")
    print(f"-- probe expectation: factory-new steering device in range, retrying continuously")
    ser = serial.Serial()
    ser.port = PORT; ser.baudrate = 115200; ser.timeout = 0.1
    ser.dtr = False; ser.rts = False
    ser.open()
    print(f"-- {time.strftime('%T')} port opened => device reset (the boot under test); draining boot frames")
    rd = FrameReader(ser)
    time.sleep(3.0)
    boot = rd.poll(2.0)
    print(f"-- drained {len(boot)} boot frame(s)")

    # Phase 1: closed network — passive listen
    print(f"-- {time.strftime('%T')} PHASE 1: listening {PHASE1_S}s with network CLOSED (no permit sent)")
    frames = rd.poll(PHASE1_S)
    inds1 = classify_indications(frames)
    for cmd, name in inds1:
        print(f"   !! indication during closed phase: {name}")
    phase1_ok = not any(cmd in JOIN_INDICATIONS for cmd, _ in inds1)

    # Phase 2: host opens the network
    print(f"-- {time.strftime('%T')} PHASE 2: sending NWK_PERMIT_JOINING(180)")
    ser.write(build_request(NWK_PERMIT_JOINING, payload=bytes([180]), tsn=0x77, seq=1))
    t0 = time.time()
    joined = None
    while time.time() - t0 < PHASE2_S:
        frames = rd.poll(2.0)
        inds = classify_indications(frames)
        hits = [(c, n) for c, n in inds if c in JOIN_INDICATIONS]
        if hits:
            joined = hits[0]
            print(f"   ++ {time.strftime('%T')} join indication: {joined[1]} (+{time.time()-t0:.0f}s after permit)")
            break
    phase2_ok = joined is not None

    print()
    print(f"{'PASS' if phase1_ok else 'FAIL'}  phase 1: no joins while closed ({PHASE1_S}s)")
    print(f"{'PASS' if phase2_ok else 'FAIL'}  phase 2: probe joined after host permit")
    print(f"== RESULT: {'ALL PASS' if (phase1_ok and phase2_ok) else 'FAILURES PRESENT'} ==")
    ser.close()
    sys.exit(0 if (phase1_ok and phase2_ok) else 1)

main()
