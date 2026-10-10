#!/usr/bin/env python3
"""Read the USB event log of the issue #17 diagnostic firmware from an
ESP32-H2/C6 coordinator. Needs only Python 3 (Linux), no extra packages.

The diagnostic firmware (esp-coordinator 1.5.77 plus diagnostics) records what
happens on its USB port during its last four starts and keeps that record in
RAM. A restart with `usj_diag.py --reset` or by the firmware itself keeps it.

Stop Zigbee2MQTT first, then run it with the port path from your Zigbee2MQTT
configuration:

    python3 issue17_log.py /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_..-if00

The port is opened the same way usj_diag.py opens it, which does not reset
the chip.

    python3 issue17_log.py --save log.bin <port>   also writes the raw log to log.bin
    python3 issue17_log.py --file log.bin          prints a saved log
"""
import argparse
import errno
import fcntl
import os
import select
import struct
import sys
import termios
import time

CMD_MODULE_VERSION = 0x0001
CMD_USJ_LOG = 0x009F
STATUS_NOT_IMPLEMENTED = 31
STATUS_BLOCKED = 2

CRC8_TABLE = bytes.fromhex(
    "ead496a8122c6e507f41033d87b9fbc5a59bd9e75d63211f300e4c72c8f6b48a"
    "744a08368cb2f0cee1df9da31927655b3b054779c3fdbf81ae90d2ec56682a14"
    "b38dcff14b75370926185a64dee0a29cfcc280be043a78466957152b91afedd3"
    "2d13516fd5eba997b886c4fa407e3c02625c1e209aa4e6d8f7c98bb50f31734d"
    "5866241aa09edce2cdf3b18f350b497717296b55efd193ad82bcfec07a440638"
    "c6f8ba843e00427c536d2f11ab95d7e989b7f5cb714f0d331c22605ee4da98a6"
    "013f7d43f9c785bb94aae8d66c52102e4e70320cb688caf4dbe5a799231d5f61"
    "9fa1e3dd67591b250a347648f2cc8eb0d0eeac922816546a457b3907bd83c1ff")


# --- ZBOSS NCP framing -------------------------------------------------------

def crc8(data):
    crc = 0
    for b in data:
        crc = CRC8_TABLE[crc ^ b]
    return crc


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def request_frame(cmd, payload, tsn, seq):
    body = bytes([0, 0, cmd & 0xFF, cmd >> 8, tsn]) + payload
    length = 7 + len(body)
    hdr = bytearray([0xDE, 0xAD, length & 0xFF, length >> 8, 0x06, 0xC0 | (seq & 3) << 2, 0])
    hdr[6] = crc8(hdr[2:6])
    c = crc16(body)
    return bytes(hdr) + bytes([c & 0xFF, c >> 8]) + body


def ack_frame(seq):
    hdr = bytearray([0xDE, 0xAD, 5, 0, 0x06, 0xC1 | (seq & 3) << 4, 0])
    hdr[6] = crc8(hdr[2:6])
    return bytes(hdr)


def take_frames(buf):
    """Remove complete frames from buf; returns (flags, body) for data frames."""
    frames = []
    while True:
        i = buf.find(b"\xde\xad")
        if i < 0:
            del buf[:max(0, len(buf) - 1)]
            return frames
        del buf[:i]
        if len(buf) < 7:
            return frames
        if crc8(buf[2:6]) != buf[6]:
            del buf[:2]
            continue
        size = 2 + (buf[2] | buf[3] << 8)
        if len(buf) < size:
            return frames
        frame = bytes(buf[:size])
        del buf[:size]
        flags = frame[5]
        if flags & 1 or size < 9:
            continue                                # link-layer ACK
        body = frame[9:]
        if crc16(body) == frame[7] | frame[8] << 8:
            frames.append((flags, body))


# --- serial port -------------------------------------------------------------

def open_port(path):
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as e:
        sys.exit("cannot open %s: %s" % (path, e.strerror))
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        sys.exit("%s is in use - stop Zigbee2MQTT first" % path)
    try:
        attrs = termios.tcgetattr(fd)
    except termios.error:
        os.close(fd)
        sys.exit("%s is not a serial port" % path)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL
    attrs[3] = 0
    attrs[4] = attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


class Link:
    def __init__(self, fd):
        self.fd = fd
        self.buf = bytearray()
        self.seq = 1
        self.tsn = 0x50

    def write(self, data):
        end = time.monotonic() + 2.0
        sent = 0
        while sent < len(data) and time.monotonic() < end:
            if select.select([], [self.fd], [], 0.2)[1]:
                try:
                    sent += os.write(self.fd, data[sent:])
                except OSError as e:
                    if e.errno != errno.EAGAIN:
                        sys.exit("write failed: %s (device gone?)" % e.strerror)
        return sent == len(data)

    def call(self, cmd, payload=b"", timeout=2.0):
        """Send one request; returns (status, payload) of its response or None."""
        self.tsn = self.tsn % 0xFE + 1
        req = request_frame(cmd, payload, self.tsn, self.seq)
        self.seq = self.seq % 3 + 1
        if not self.write(req):
            return None
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if not select.select([self.fd], [], [], 0.1)[0]:
                continue
            try:
                self.buf += os.read(self.fd, 4096)
            except OSError as e:
                if e.errno == errno.EAGAIN:
                    continue
                sys.exit("read failed: %s (device gone?)" % e.strerror)
            for flags, body in take_frames(self.buf):
                self.write(ack_frame(flags >> 2 & 3))
                if len(body) >= 7 and body[1] == 1 and body[2] | body[3] << 8 == cmd and body[4] == self.tsn:
                    return body[6], body[7:]
        return None


def read_image(path):
    fd = open_port(path)
    link = Link(fd)
    try:
        ver = None
        for _ in range(3):
            ver = link.call(CMD_MODULE_VERSION)
            if ver is not None:
                break
        if ver is None:
            sys.exit("no answer from the coordinator firmware. Run usj_diag.py <port> and post its output,\n"
                     "then usj_diag.py --reset <port> and run this script again: the log survives that reset.")
        time.sleep(0.3)     # the firmware polls the USB controller every 50 ms; let it record this open
        image = bytearray()
        total = None
        while total is None or len(image) < total:
            reply = None
            for _ in range(3):
                reply = link.call(CMD_USJ_LOG, struct.pack("<I", len(image)))
                if reply is not None:
                    break
            if reply is None:
                sys.exit("the log read stopped at byte %d - run the script again" % len(image))
            status, data = reply
            if status == STATUS_NOT_IMPLEMENTED:
                sys.exit("this firmware has no event log - it is not the issue #17 diagnostic build")
            if status == STATUS_BLOCKED:
                sys.exit("the firmware runs in safe mode and has no event log - not the issue #17 diagnostic build")
            if status != 0 or len(data) < 8:
                sys.exit("log read failed, status %d" % status)
            size, n = struct.unpack_from("<II", data)
            if total is None:
                total = size
            if n == 0:
                break
            image += data[8:8 + n]
        return bytes(image[:total]), time.time()
    finally:
        termios.tcflush(fd, termios.TCIFLUSH)
        os.close(fd)


# --- decoding ----------------------------------------------------------------

LIVE = struct.Struct("<IHHIIII4B7IIIII")
REC = struct.Struct("<IBBHII")
SLOT_HDR = struct.Struct("<IIII")
STORE_HDR = struct.Struct("<IIIII12x")

FLAG_KICK, FLAG_UART, FLAG_SAFE, FLAG_KEPT, FLAG_SNAPSHOT = 1, 2, 4, 8, 16

ESP_RESET = ["unknown", "power-on", "external pin", "software restart", "panic", "interrupt watchdog",
             "task watchdog", "other watchdog", "deep sleep", "brownout", "SDIO", "USB", "JTAG", "eFuse",
             "power glitch", "CPU lockup"]
ROM_RESET = {0x01: "power-on/brownout", 0x03: "core software reset", 0x05: "deep sleep", 0x07: "MWDT0",
             0x08: "MWDT1", 0x09: "RTC WDT", 0x0B: "CPU MWDT0", 0x0C: "CPU software reset", 0x0D: "CPU RTC WDT",
             0x0F: "brownout", 0x10: "system RTC WDT", 0x11: "CPU MWDT1", 0x12: "super WDT",
             0x14: "eFuse CRC", 0x15: "USB RTS/DTR reset", 0x16: "USB JTAG reset", 0x17: "power glitch",
             0x18: "CPU JTAG reset"}
REGS_WHY = ["at boot", "TX waiting", "before kick", "TX event lost", "at readout"]
TXW = ["host is not reading the port", "IN FIFO busy while the host polls", "IN FIFO free, IN_EMPTY event missing"]


def reset_name(esp, rom):
    name = ESP_RESET[esp] if esp < len(ESP_RESET) else "reason %d" % esp
    return "%s (ROM 0x%02x %s)" % (name, rom, ROM_RESET.get(rom, "?"))


def usj_state(raw, ena, ep1):
    queued = bool(ena & 0x08)
    free = bool(ep1 & 0x02)
    event = bool(raw & 0x08)
    if not queued:
        tx = "TX idle"
    elif free and not event:
        tx = "TX queued, IN FIFO free, IN_EMPTY event MISSING"
    elif free:
        tx = "TX queued, IN FIFO free, event pending"
    else:
        tx = "TX queued, IN FIFO holds a packet for the host"
    return tx


def regs_text(c, d, b):
    raw, ena = c & 0xFFFF, c >> 16
    ep1, ctl, in1 = d & 0xF, d >> 4 & 0xF, d >> 16
    return ("%s | raw=%04x ena=%03x ep1=%x in1=%04x (state %d wr %d rd %d) DTR=%d RTS=%d SOF %d"
            % (usj_state(raw, ena, ep1), raw, ena, ep1, in1, in1 & 3, in1 >> 2 & 0x7F, in1 >> 9 & 0x7F,
               ctl >> 1 & 1, ctl & 1, b))


def tasks_text(c):
    if not c:
        return ""
    st = [chr(c >> s & 0xFF) if 32 < (c >> s & 0xFF) < 127 else "?" for s in (0, 8, 16, 24)]
    return "tasks main:%s zigbee:%s transport:%s idle:%s" % tuple(st)


def event_text(t, a, b, c, d):
    if t == 1:
        flags = d & 0xFFFF
        rev = d >> 16
        return ("START #%d: reset by %s, chip v%d.%d, safe mode %s, earlier log %s"
                % (c, reset_name(a, b), rev // 100, rev % 100, "on" if flags & FLAG_SAFE else "off",
                   "kept" if flags & FLAG_KEPT else "not kept (new log)"))
    if t == 2:
        why = REGS_WHY[a] if a < len(REGS_WHY) else str(a)
        return "USB registers %s: %s" % (why, regs_text(c, d, b))
    if t == 3:
        return "USB bus reset #%d" % b
    if t == 4:
        return "USB bus %s (SOF %d)" % ("active" if a else "idle - no host traffic", b)
    if t == 5:
        return "host port lines DTR=%d RTS=%d%s" % (a, b, " (port opened)" if a and b else
                                                     " (port closed)" if not a and not b else "")
    if t == 6:
        return "replies routed to %s (route changes: %d)" % ({0: "none", 1: "USB", 2: "UART"}.get(a, a), b)
    if t == 7:
        return "host sends data (total %d bytes in %d reads)" % (c, d)
    if t == 8:
        return "USB write did not fit, TX buffer full (writes ok %d, failed %d)" % (c, d)
    if t == 9:
        return "USB writes fit again (writes ok %d, failed %d)" % (c, d)
    if t == 10:
        return "TX waiting %.1f s: %s (host IN polls %d)" % (c / 1000, TXW[a] if a < len(TXW) else a, b)
    if t == 11:
        return ("TX KICK #%d: data waited %.1f s, event missing for %.1f s -> zero-length packet sent "
                "(host IN polls %d)" % (a, c / 1000, d / 1000, b))
    if t == 12:
        return "TX STALL: data waits %.1f s, event missing for %.1f s (kick disabled)" % (c / 1000, d / 1000)
    if t == 13:
        return "TX drained after %.1f s (kicks %d, host IN polls %d)" % (c / 1000, a, b)
    if t == 14:
        return "APP STALL: event pump has not run for 5 s (%s, events in/out %d/%d)" % (tasks_text(c), d & 0xFFFF, d >> 16)
    if t == 15:
        return "app event pump running again after %.1f s" % (c / 1000)
    if t == 16:
        return ("status: bus %s, DTR=%d RTS=%d, TX %s, host IN polls %d | received %d bytes, writes ok %d"
                % ("active" if a & 1 else "idle", a >> 1 & 1, a >> 2 & 1, "queued" if a & 8 else "idle", b, c, d))
    if t == 17:
        return "log read by the host" + (" (%s)" % tasks_text(c) if c else "")
    return "event %d a=%d b=%d c=%d d=%d" % (t, a, b, c, d)


def decode(image, read_at=None):
    if len(image) < LIVE.size:
        sys.exit("log image too short (%d bytes)" % len(image))
    live = LIVE.unpack_from(image)
    (magic, layout, hsize, fw, rev, uptime, flags, cur, nslots, head, ring,
     raw, ena, ep1, in1, out1, fram, ctl, kicks, rx, w_ok, w_fail) = live
    if magic != 0x4C4A5355 or layout != 1:
        sys.exit("unknown log format (magic %08x layout %d)" % (magic, layout))
    nrec = head + ring
    slot_size = SLOT_HDR.size + nrec * REC.size
    out = []
    when = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(read_at)) if read_at else "unknown"
    out.append("esp-coordinator %d.%d.%d, issue #17 diagnostic build r%d, TX kick %s, NCP UART %s"
               % (fw >> 24, fw >> 16 & 0xFF, fw & 0xFFFF, rev, "on" if flags & FLAG_KICK else "off",
                  "on" if flags & FLAG_UART else "off"))
    out.append("read at %s (host clock), coordinator up %.1f s, kicks this start %d, received %d bytes, "
               "writes ok %d failed %d" % (when, uptime / 1000, kicks, rx, w_ok, w_fail))
    out.append("USB now: %s" % regs_text((raw & 0xFFFF) | (ena & 0xFFFF) << 16,
                                         (ep1 & 0xF) | (ctl & 0xF) << 4 | (in1 & 0xFFFF) << 16, fram & 0x7FF))
    base = hsize
    _, _, boot_seq, _, _ = STORE_HDR.unpack_from(image, base)
    slots = []
    for i in range(nslots):
        off = base + STORE_HDR.size + i * slot_size
        seq, count, alive, xsum = SLOT_HDR.unpack_from(image, off)
        if not seq:
            continue
        recs = [REC.unpack_from(image, off + SLOT_HDR.size + k * REC.size) for k in range(nrec)]
        check = 0
        for k in range(nrec):
            for w in struct.unpack_from("<4I", image, off + SLOT_HDR.size + k * REC.size):
                check ^= w
        slots.append((seq, i, count, alive, xsum == check, recs))
    slots.sort()
    # Host-clock estimate: the current start began uptime ms before the read;
    # an earlier start ended about when the next one began, unless power was lost.
    start_at = {}
    if read_at:
        newest_first = slots[::-1]
        t0 = read_at - uptime / 1000
        for n, (seq, i, count, alive, ok, recs) in enumerate(newest_first):
            start_at[seq] = t0
            boot = recs[0] if count else None
            if boot is None or boot[1] != 1 or boot[2] == 1 or n + 1 == len(newest_first):
                break                                       # power-on: time switched off unknown
            t0 -= newest_first[n + 1][3] / 1000 + 0.25    # + reset, ROM and bootloader
    for seq, i, count, alive, ok, recs in slots:
        out.append("")
        label = "this start" if i == cur else "%d start(s) before this one" % (boot_seq - seq)
        out.append("== start #%d (%s): ran %.1f s, %d events%s%s" % (
            seq, label, alive / 1000 if i != cur else uptime / 1000, count,
            "" if ok or (i == cur and not flags & FLAG_SNAPSHOT) else
            ", CHECKSUM MISMATCH (RAM not kept, or reset during a write) - entries may be garbage",
            ", %d in the middle not kept" % (count - nrec) if count > nrec else ""))
        if count <= head:
            order = list(range(count))
        else:
            n_ring = min(count - head, ring)
            newest = (count - head - 1) % ring
            order = list(range(head)) + [head + (newest - n_ring + 1 + k) % ring for k in range(n_ring)]
        shown_gap = False
        for n, k in enumerate(order):
            t_ms, typ, a, b, c, d = recs[k]
            if count > nrec and n == head and not shown_gap:
                out.append("   ... %d events not kept ..." % (count - nrec))
                shown_gap = True
            stamp = "%9.3f s" % (t_ms / 1000)
            if seq in start_at:
                try:
                    stamp += time.strftime("  ~%H:%M:%S", time.localtime(start_at[seq] + t_ms / 1000))
                except (ValueError, OverflowError, OSError):
                    pass
            out.append("  %s  %s" % (stamp, event_text(typ, a, b, c, d)))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description="Read the USB event log of the issue #17 diagnostic firmware.")
    ap.add_argument("port", nargs="?", help="serial port of the coordinator")
    ap.add_argument("--save", metavar="FILE", help="also write the raw log to FILE")
    ap.add_argument("--file", metavar="FILE", help="print a log saved with --save")
    args = ap.parse_args()
    if args.file:
        with open(args.file, "rb") as f:
            data = f.read()
        read_at = None
        if data[:8] == b"USJLOG1\n":
            read_at = struct.unpack_from("<d", data, 8)[0]
            data = data[16:]
        print(decode(data, read_at))
        return
    if not args.port:
        ap.error("give the serial port, or --file")
    print("issue17_log: %s -> %s" % (args.port, os.path.realpath(args.port)))
    image, read_at = read_image(args.port)
    if args.save:
        with open(args.save, "wb") as f:
            f.write(b"USJLOG1\n" + struct.pack("<d", read_at) + image)
    print(decode(image, read_at))


if __name__ == "__main__":
    main()
