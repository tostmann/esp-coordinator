#!/usr/bin/env python3
"""Move a Zigbee network from a USB-stick flash dump (tostmann/esp-coordinator)
into an ESPHome zboss_ncp build.

The stick keeps its network in partition 'zb_storage' (ZBOSS NVRAM); the
ESPHome build has 'zb_storage' of the same size plus 'zb_nvs', a placeholder
the firmware never mounts. This writes the stick's zb_storage and an erased
zb_nvs as files, plus the esptool arguments for the new offsets. The stick's
'nvs' (on a coex stick including its WiFi credentials) is not carried over,
so it never ends up in backups. Same chip = same IEEE address, so the network
and its paired devices carry over.

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
    so, ss = old["zb_storage"]
    do, ds = new["zb_storage"]
    if ss != ds:
        sys.exit(f"zb_storage differs in size ({ss:#x} -> {ds:#x})")
    f = out / "zb_storage.bin"
    f.write_bytes(dump[so:so + ss])
    args += [f"{do:#x}", str(f)]
    print(f"zb_storage@{so:#x} -> zb_storage@{do:#x} ({ss // 1024} KB)", file=sys.stderr)
    do, ds = new["zb_nvs"]
    f = out / "zb_nvs.bin"
    f.write_bytes(b"\xff" * ds)   # erased flash
    args += [f"{do:#x}", str(f)]
    print(f"zb_nvs@{do:#x} erased ({ds // 1024} KB)", file=sys.stderr)
    print(" ".join(args))


if __name__ == "__main__":
    main()
