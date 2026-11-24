#!/usr/bin/env python3
"""Raw network backup / restore of a ZBOSS NCP over TCP.

usage: ncp_backup.py <host> backup <file> [port]
       ncp_backup.py <host> restore <file> [port] [--yes]

backup:  GET_NETWORK_BACKUP (0x0099) in chunks; writes the 40 KB raw image
         (NVS half + zb_storage), as html/zboss_backup.js does.
restore: RESTORE_NETWORK (0x009A) in 128-byte chunks (offset, total, data);
         the NCP checks the image and reboots. Same chip = same IEEE address.
         Asks for confirmation first (--yes skips it).

backup refuses to overwrite an existing file, so a swapped command harms
nothing.

Disconnect Zigbee2MQTT/ZHA first: the NCP serves one host at a time.
Framing from ncp_probe.py.
"""
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ncp_probe import build_request, send_ack  # noqa: E402

GET_NETWORK_BACKUP = 0x0099
RESTORE_NETWORK = 0x009A
EXPECTED_RAW_SIZE = 0xA000
RESTORE_CHUNK = 128   # host->NCP frames must fit the NCP's 1 KB RX buffer


class Ncp:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = bytearray()
        self.seq = 0
        self.tsn = 0
        for f in self._frames(1.5):          # boot frames
            if not f[5] & 0x01:
                send_ack(self.sock, f)

    def _frames(self, timeout_s):
        """Frames that arrive within timeout_s (used for the boot frames)."""
        end = time.time() + timeout_s
        out = []
        while time.time() < end:
            f = self._next_frame(end - time.time())
            if f:
                out.append(f)
        return out

    def _next_frame(self, timeout_s):
        end = time.time() + max(timeout_s, 0)
        while True:
            i = self.buf.find(b"\xde\xad")
            if i > 0:
                del self.buf[:i]
            if i >= 0 and len(self.buf) >= 4:
                plen = self.buf[2] | (self.buf[3] << 8)
                if len(self.buf) >= 2 + plen:
                    f = bytes(self.buf[:2 + plen])
                    del self.buf[:2 + plen]
                    return f
            left = end - time.time()
            if left <= 0:
                return None
            self.sock.settimeout(min(left, 0.5))
            try:
                chunk = self.sock.recv(2048)
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError("NCP closed the connection")
            self.buf += chunk

    def request(self, cmd_id, payload=b"", timeout_s=10.0):
        self.seq = (self.seq % 3) + 1
        self.tsn = (self.tsn + 1) & 0xFF
        self.sock.sendall(build_request(cmd_id, payload, tsn=self.tsn, seq=self.seq))
        end = time.time() + timeout_s
        while time.time() < end:
            f = self._next_frame(end - time.time())
            if f is None:
                break
            if f[5] & 0x01 or len(f) < 14:   # ACK / too short
                continue
            send_ack(self.sock, f)
            pl = f[9:]
            if (pl[2] | (pl[3] << 8)) != cmd_id or pl[4] != self.tsn:
                continue                     # indication or stale response
            body = pl[5:]
            return body[0], body[1], body[2:]
        raise TimeoutError(f"no response to 0x{cmd_id:04X}")


def backup(ncp, path):
    offset, total, parts = 0, None, []
    t0 = time.time()
    while total is None or offset < total:
        cat, status, data = ncp.request(GET_NETWORK_BACKUP, struct.pack("<I", offset))
        if status != 0:
            sys.exit(f"GET_NETWORK_BACKUP failed at offset {offset}: status {cat}/{status}")
        total, length = struct.unpack("<II", data[:8])
        if length == 0:
            sys.exit(f"empty chunk at offset {offset}")
        parts.append(data[8:8 + length])
        offset += length
    raw = b"".join(parts)
    with open(path, "xb") as f:      # never overwrite, also not a file made meanwhile
        f.write(raw)
    print(f"backup: {len(raw)} bytes in {time.time() - t0:.1f} s -> {path}")


def read_image(path):
    if not Path(path).is_file():
        sys.exit(f"{path}: no such file")
    raw = Path(path).read_bytes()
    if len(raw) != EXPECTED_RAW_SIZE:
        sys.exit(f"{path}: {len(raw)} bytes, expected {EXPECTED_RAW_SIZE}")
    return raw


def restore(ncp, path, raw):
    t0 = time.time()
    for offset in range(0, len(raw), RESTORE_CHUNK):
        chunk = raw[offset:offset + RESTORE_CHUNK]
        cat, status, _ = ncp.request(RESTORE_NETWORK, struct.pack("<II", offset, len(raw)) + chunk)
        if status != 0:
            sys.exit(f"RESTORE_NETWORK failed at offset {offset}: status {cat}/{status}")
    print(f"restore: {len(raw)} bytes in {time.time() - t0:.1f} s; the NCP reboots")


def main():
    argv = sys.argv[1:]
    yes = "--yes" in argv
    argv = [a for a in argv if a != "--yes"]
    if len(argv) not in (3, 4) or argv[1] not in ("backup", "restore"):
        sys.exit(__doc__)
    host, action, path = argv[:3]
    port = int(argv[3]) if len(argv) > 3 else 6638
    if action == "backup":
        if Path(path).exists():
            sys.exit(f"{path} exists; choose another file name (backup never overwrites)")
        backup(Ncp(host, port), path)
        return
    raw = read_image(path)
    if not yes:
        try:
            answer = input(f"Replace the Zigbee network on {host}:{port} with {path}? [y/N] ")
        except EOFError:          # no terminal: counts as "no"
            answer = ""
        if answer.strip().lower() not in ("y", "yes"):
            sys.exit("restore cancelled")
    restore(Ncp(host, port), path, raw)


if __name__ == "__main__":
    main()
