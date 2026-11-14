#!/usr/bin/env python3
"""Move a Zigbee network from a USB-stick flash dump (tostmann/esp-coordinator)
into an ESPHome zboss_ncp build.

The stick keeps its network in partition 'zb_storage' (ZBOSS NVRAM) and the
NVS half of a network backup in 'nvs'; the ESPHome build has 'zb_storage' and
'zb_nvs' of the same sizes. This writes both halves as files, plus the
esptool arguments for the new offsets. Same chip = same IEEE address, so the
network and its paired devices carry over.

usage: migrate_stick.py <stick_dump.bin> <esphome partition-table.bin> <out_dir>
"""
import struct
import sys
from pathlib import Path


def partitions(table: bytes) -> dict[str, tuple[int, int]]:
    out = {}
    for i in range(0, len(table), 32):
        e = table[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        off, size = struct.unpack("<II", e[4:12])
        out[e[12:28].split(b"\0")[0].decode()] = (off, size)
    return out


def main() -> None:
    dump = Path(sys.argv[1]).read_bytes()
    new = partitions(Path(sys.argv[2]).read_bytes())
    old = partitions(dump[0x8000:0x9000])
    out = Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    args = []
    for src, dst in (("zb_storage", "zb_storage"), ("nvs", "zb_nvs")):
        so, ss = old[src]
        do, ds = new[dst]
        if ss != ds:
            sys.exit(f"{src} ({ss:#x}) and {dst} ({ds:#x}) differ in size")
        f = out / f"{dst}.bin"
        f.write_bytes(dump[so:so + ss])
        args += [f"{do:#x}", str(f)]
        print(f"{src}@{so:#x} -> {dst}@{do:#x} ({ss // 1024} KB)", file=sys.stderr)
    print(" ".join(args))


if __name__ == "__main__":
    main()
