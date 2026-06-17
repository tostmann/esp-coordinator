# busware.de ESP32 (ZBOSS) Coordinator

A highly optimized ZBOSS NCP Serial Protocol implementation for ESP32-C6/H2 modules, tailored specifically for **Zigbee2MQTT**. 

This firmware transforms any cheap, standard ESP32-C6 development board into an enterprise-grade Zigbee Coordinator that rivals or exceeds established commercial adapters.

[Protocol specification](https://wiki.homed.dev/files/9/95/ZBOSS_NCP_Serial_Protocol.pdf)

## Why ESP32-C6? (Competitive Positioning)

The ESP32-C6 disrupts the traditional Zigbee Coordinator market (dominated by Texas Instruments Z-Stack and Silicon Labs EZSP) by offering massive hardware capabilities at a fraction of the cost. 

**Virtually ANY ESP32-C6 board on the market (including generic $3 boards from China) works out-of-the-box as a high-end Coordinator.** You do not need specialized "coordinator" dongles anymore.

### 🚀 Key Advantages Over the Competition

1. **Massive Capacity (200 Nodes)**
   Older chips like the TI CC2531 max out at 20-40 direct children due to tiny 8KB RAM. The ESP32-C6 boasts **512KB SRAM**. This firmware is hardcoded to support **200 direct nodes** natively, allowing you to build massive, star-shaped networks without memory exhaustion, directly competing with the expensive TI CC2652 range.

2. **Native "Long-Range" Transmit Power (+20 dBm)**
   While standard Zigbee chips output +5 dBm and require expensive external Power Amplifiers (the "P" in CC2652P) to reach high range, the ESP32-C6 has an integrated power amplifier on the silicon. It natively transmits at up to **+20 dBm** out of the box, making it a true Long-Range coordinator.

3. **Native Bare-Metal Backups (since v1.1.0, hybrid format since v1.2.x)**
   Unlike Z-Stack or EZSP, which require hundreds of lines of complex parsing scripts to extract individual network keys and tables from RAM, this firmware implements a **Chunked Raw NVRAM Transfer** plus, since v1.2.x, a **structured backup** in the Zigbee Alliance Universal NWK Backup JSON format (PAN, ExtPAN, channel, NWK key, live frame counter, neighbor table — small, human-inspectable, format-independent of firmware version). Zigbee2MQTT consumes both as one hybrid `coordinator_backup.json`: structured fields for inspection and portability, raw NVRAM for byte-true restore. Swap a broken ESP32-C6 for a new one, hit restore, and paired devices come back without re-pairing.

4. **All-in-One SoC (Wi-Fi 6 + Zigbee)**
   Traditional network coordinators require two chips: a Zigbee radio (TI/SiLabs) and a Wi-Fi bridge (ESP32). The ESP32-C6 has both radios built into a single chip, drastically reducing hardware complexity for networked gateways.

5. **Modern ZBOSS Stack**
   The commercial, highly-certified ZBOSS stack provides an extremely robust alternative to the often complex and legacy-burdened EZSP protocol.


## Recent Fixes & Improvements

This project is an actively maintained, heavily optimized evolution of the original esp-coordinator. As of 2026-05, the active upstream lives in this fork; the original `andryblack/esp-coordinator` repository is **archived and read-only** and its README points here. If you arrived via a search hit on one of the archived `andryblack` issues, see [`LEGACY_ANDRYBLACK_ISSUES.md`](LEGACY_ANDRYBLACK_ISSUES.md) for the current status of each report.

### v1.3.x — UART Host Transport, XIAO ESP32-C6 Support, NCP_RESET Protocol Fix

* **NEW: UART host transport, in parallel to USB** ([#2](https://github.com/tostmann/esp-coordinator/issues/2), [discussion #1](https://github.com/tostmann/esp-coordinator/discussions/1)): the NCP frame stream is now additionally available on a hardware UART — **TX = GPIO22, RX = GPIO23 (= D4/D5 on the Seeed XIAO ESP32-C6), 115200 8N1, no flow control** (all configurable via `idf.py menuconfig` → `NCP_UART_*`). This lets the coordinator hang directly off a host board's TTL serial header (OpenWRT routers, Raspberry Pi GPIO header, ser2net boxes) without using USB at all. Both interfaces are always live; responses follow whichever interface last delivered a **valid NCP frame** — random bytes from port probers such as ModemManager can neither steal the link nor corrupt the stream (signature-gated routing). Running two talking hosts at once is not supported. Validated end-to-end over a Raspberry Pi header UART with both `zigbee2mqtt` (`adapter: zboss`) and `zigpy-zboss`, including network formation, device pairing and the full 40 KB backup pull. Bonus: with an external USB-UART adapter or a host header UART, the host port survives the chip's resets — the USB-CDC `inReset` quirk class disappears entirely on this path.
* **NEW: Seeed XIAO ESP32-C6 external-antenna option**: `CONFIG_NCP_XIAO_EXT_ANTENNA` drives the XIAO's RF-switch control pins (GPIO3 enable, GPIO14 select) to route RF to the external U.FL connector — useful for a coordinator that needs real range. Default off; by default the firmware now leaves both switch pins in a defined passive INPUT state, so the board's pulldown selects the on-board ceramic antenna and the binary stays a no-op on every other board.
* **NCP_RESET protocol-semantics fix** (important for `reset` / factory-reset / restore flows): the firmware no longer sends an "OK" response *before* rebooting. Hosts treat that response as "reset complete" and continue immediately — everything they sent in the window before the actual reboot was killed mid-flight (this was the root cause of the long-standing "backup timeout after formation/restore" failures). Now the reboot happens right away and the **boot-ready frame after the reboot is the response** — exactly what the host's wildcard NCP_RESET matcher expects. Factory-reset → re-form and restore → resume flows now complete deterministically.
* **NCP_RESET-over-USB freeze fixed**: the v1.1.x USB "phy detach" register writes turned out to hard-freeze the ESP32-C6 on current ESP-IDF builds (bus stall, only a power-cycle recovers) — every NCP_RESET over USB bricked the stick until replug. The writes are removed; NVRAM/factory erase now runs at the start of the *next* boot (parked via RTC memory), before the radio is active.
* **Full-spec `NWK_FORMATION` response** (`NWKAddr` + `PANID` + channel page + channel): fixes network formation against hosts that parse the complete response layout — notably `zigpy-zboss` ≥ 2.x / Home Assistant ZHA.
* **Consistent point-in-time backups**: `GET_NETWORK_BACKUP` now serves all chunks from a RAM snapshot taken at the first chunk, so the 40 KB image can no longer mutate while it is being pulled.
* **Link-layer fix**: a cross-task sequence-number race between ACK generation and data frames (duplicate `packet_seq` on the wire → host-side "Unexpected packet sequence") is closed.

### v1.2.33 — Code-Review Hardening

A full whole-codebase review fixed two memory-safety criticals and a batch of robustness/correctness issues. Every change was adversarially re-verified, and the build was validated end-to-end on hardware (network formation → device interview → backup) against the Zigbee2MQTT `adapter:zboss` host.

* **Memory-safety (critical):** fixed an out-of-bounds stack write + info-leak in the `APSDE_DATA_REQ` confirm serializer (fired on *every* APS confirm), and a `packet_len < 7` length underflow in the frame parser where a single crafted frame could trigger a multi-GB out-of-bounds read and a remote crash/restart-loop. A new length guard in `on_rx_data` closes six downstream over-read paths at one point. The host-side JS parser (`html/zboss_backup.js`) carries the matching min-length guard.
* **Cross-task data races:** the request-resolver slot table and the `single_cmd_delayed` slot (`NWK_FORMATION` / `NWK_START_WITHOUT_FORMATION`) are now mutex-guarded with atomic take-and-clear, removing torn-read races between the app task and the ZBOSS task.
* **Defined-behaviour access:** all unaligned 16/32-bit reads/writes in the ZDO/APSDE response serializers converted to `memcpy`.
* **Robustness:** RESTORE paths now check every `esp_partition` result and abort cleanly instead of half-wiping NVS then rebooting; `nvs_flash_init` does erase-and-retry instead of boot-looping on a host-restored bad layout; the transport TX capacity check moved inside the mutex (no two-writer frame corruption); app/protocol buffer-size drift closed.
* **`SET_EXTENDED_PAN_ID` byte order** now mirrors `GET_EXTENDED_PAN_ID`'s 8-byte reversal so the pair round-trips — verified on hardware against a non-palindrome ext PAN ID.
* **Hygiene:** atomic version-file writes, validated `version.txt`, host-side ACK + CRC validation in the web flasher, and the never-implemented UART transport option removed (USB-Serial/JTAG only).

### v1.2.x — Structured Backup + Hybrid Restore

* **Structured backup** (NCP commands `0x009B GET_STRUCTURED_BACKUP` / `0x009C RESTORE_STRUCTURED_BACKUP`): the coordinator now exports its identity (`panID`, `extendedPanID`, `channel`, `nwkUpdateId`, coordinator IEEE, NWK key, live NWK outgoing frame counter, neighbor table) as a small TLV image (~80 B + 16 B/device). Magic `'ZBSB'`, format version 1.
* **Hybrid `coordinator_backup.json`**: the Z2M-fork backup adapter calls `GET_STRUCTURED_BACKUP` first to populate the **Zigbee Alliance Universal NWK Backup** JSON shape (live key, live frame counter, devices array), then appends the raw 40 KB NVRAM blob in `stack_specific.zboss.raw_nvram` for byte-true restore. Structured fields are inspectable, portable, and format-independent of firmware version; raw_nvram is the actual restore source today.
* **Live-verified end-to-end** (2026-05-24): backup → `esptool erase_flash` → reflash factory.bin → start Z2M → end-devices reconnect at full link quality, without re-pair.
* **Spec follow-up filed**: [espressif/esp-zigbee-sdk#818](https://github.com/espressif/esp-zigbee-sdk/issues/818) asks Espressif to document NVRAM backup/restore commands in the ZBOSS NCP Serial Protocol, so a future upstream `zigbee-herdsman` PR can remove the `throw NOT_SUPPORTED` paths in `adapter:zboss`.

### v1.1.1+ — Reliability & Interop

* **Cold-boot panID/channel race FIXED ([#5](https://github.com/andryblack/esp-coordinator/issues/5) / [#19](https://github.com/andryblack/esp-coordinator/issues/19) / [z2m#26152](https://github.com/Koenkk/zigbee2mqtt/issues/26152)):** The ZBOSS dispatch task is now started at boot from `app::start_int`, not lazily from `NWK_FORMATION` / `NWK_START_WITHOUT_FORMATION`. Without this, z2m's first `GET_JOINED` / `GET_PAN_ID` queries returned the uninitialised 0xFFFF / 0xFF defaults, z2m saw a "different" network than its configured options and called `formNetwork()` — silently wiping every paired device. Verified live: persisted `panID`, `extendedPanID`, and `channel` come back correctly on every cold boot.
* **NCP_RESET deferred task + USB phy detach:** Long-running parts of factory-reset (`zb_nvram_erase`, `zb_bdb_reset_via_local_action`) moved off the request-handling task so the matching-tsn response goes out on the wire immediately. Before `esp_restart()`, the firmware now forcibly disables `USB_SERIAL_JTAG_CONF0_REG`'s `DP_PULLUP` + `USB_PAD_ENABLE` for 800 ms — the host CDC layer sees a real disconnect, which is what herdsman's `onPortClose` needs to release its `inReset` flag. (For end-to-end factory-reset, the host also needs the matching change in `tostmann/zigbee2mqtt` `scripts/patch_zboss.js`; see [ZIGBEE2MQTT.md](ZIGBEE2MQTT.md).)
* **Tuya Simple_Desc_rsp intercept ([esp-zigbee-sdk#485](https://github.com/espressif/esp-zigbee-sdk/issues/485)):** The prebuilt ZBOSS stack does a strict frame-length check on ZDP Simple_Desc_rsp and silently drops responses with trailing bytes, breaking interview for some Tuya devices (OUI prefix `0x70b3d5...`, e.g. TS011F). The firmware now intercepts cluster 0x8004 indications, tolerantly re-parses them, and synthesises a clean response to the host. First known userspace workaround of #485. Live-verified on `_TZ3000_w0qqde0g`.
* **Audit pass:** 17 fixes across critical/high/medium severity — APSDE bound checks, ZDO request slot lifecycle, RESTORE_NETWORK chunked-transfer hardening, real firmware-version reporting via `GET_MODULE_VERSION`, etc. See `git log`.

### v1.1.0 — Foundational

* **Native NVRAM Backup & Restore:** Complete firmware-side implementation of custom NCP Commands `0x0099` and `0x009A`, supporting chunked Z2M backups (40 KB image = nvs + zb_storage partitions).
* **NVRAM Persistence Fix:** Coordinator accurately resumes its network from NVRAM on boot, maintaining pairings and frame counters across restarts.
* **Manufacturer Code Workaround:** `ZDO_SET_NODE_DESC_MANUF_CODE` implementation allows Z2M to emulate the manufacturer code dynamically.
* **Network Scaling:** Tables and memory dynamically optimized for 200 nodes.
* **Dynamic TX Power:** Supports dynamic adjustment of the transmission power up to 20 dBm.
* **Permit Join Handling:** Added full support for the `NWK_PERMIT_JOINING (0x0404)` command.
* **Modern ZBOSS SDK:** Fully ported to ZBOSS SDK v1.6.x.


## 🧪 Experimental: native Wi-Fi (coexistence, Mode B)

A separate **experimental** build (branch [`wifi-coex`](https://github.com/tostmann/esp-coordinator/tree/wifi-coex)) lets the coordinator **join your Wi-Fi itself** and expose the ZBOSS NCP over an on-device **TCP server** — no USB host, no ser2net/socat bridge. Zigbee2MQTT then connects with `port: tcp://<device-ip>:6638` and `adapter: zboss`.

👉 **[Experimental WiFi-Coex flasher](https://install.busware.de/zboss/coex/)**

* **Provisioning:** flash in the browser, then enter your Wi-Fi in the same flow (Improv-Serial). The device saves it and reboots onto your network. After provisioning, the flasher's "Visit Device" link opens a small setup page served by the coordinator itself.
* **Re-configure Wi-Fi later:** power-cycle and, within the first 120 s, use "Change Wi-Fi" in the flasher — no reflash needed.
* **Discovery:** mDNS `esp-zboss-coord.local:6638`, or the device's DHCP IP.
* **Required host image:** `ghcr.io/tostmann/zigbee2mqtt-esp32:latest` — it carries the longer timeouts the single-radio TCP link needs.

> ⚠️ **Why "experimental":** the ESP32-C6 has a single 2.4 GHz radio time-shared between Wi-Fi and 802.15.4 Zigbee (software coexistence). Espressif rates the "Wi-Fi-STA + always-on coordinator" case as supported-but-unstable, so coexistence quality depends on your RF environment. Use a spare stick / test network, **not** a production setup — and please report how it went on [**Discussion #1**](https://github.com/tostmann/esp-coordinator/discussions/1). Backup/restore over TCP is degraded (use the stable USB build for a full backup).

The stable USB / UART firmware described in this README (and the main flasher) is unchanged.


## Zigbee2MQTT Integration & Hardware Migration

While this coordinator works perfectly with the standard Zigbee2MQTT release, **we highly recommend using our customized Docker image** (`ghcr.io/tostmann/zigbee2mqtt-esp32:latest`).

Our custom image unlocks **Native NVRAM Snapshots**, allowing Zigbee2MQTT to automatically stream the ESP32's complete NVRAM over the serial protocol. This means if your hardware breaks, you can just plug in a new ESP32 and Z2M will automatically transfer your latest network state (including frame counters) to the new chip without having to re-pair any devices.

### Which Zigbee2MQTT do I need?

| Setup | Standard `koenkk/zigbee2mqtt` | `ghcr.io/tostmann/zigbee2mqtt-esp32` |
|---|---|---|
| **Fresh install** (empty chip, first start) | ✅ Works | ✅ Works |
| **Existing network**, `configuration.yaml` matches device's persisted `channel` / `panID` / `extendedPanID` | ✅ Works — devices stay paired across reboots (cold-boot panID race fix) | ✅ Works |
| **Existing network**, `configuration.yaml` channel/panID **differs** from device — e.g. migrating between coordinators, or re-aiming at a different network | ⚠️ z2m calls `reset(FactoryReset)` and hangs on a 10 s timeout; the chip's NVRAM gets erased in the process, so paired devices are effectively lost on next start | ✅ Reset → factory-reset → re-form flow completes end-to-end, then devices can be re-paired (or restored from a backup `coordinator_backup.json`) |
| **Coordinator hardware migration** (swap ESP32 chip, keep network) | ❌ No native backup/restore support | ✅ Raw-NVRAM transfer over the wire — paired devices come back without re-pairing if the snapshot is recent |

**TL;DR**: if your `configuration.yaml` already matches the device's persisted network, the standard z2m works fine.  If you need to change network parameters, migrate hardware, or just want the safety net of automated NVRAM snapshots, use the `tostmann/zigbee2mqtt-esp32` Docker image.

The relevant host-side patches (`RESTORE_NETWORK` adapter integration plus the `onPackage`-during-`inReset` fix that closes the factory-reset hang) live in `scripts/patch_zboss.js` on the [`tostmann/zigbee2mqtt`](https://github.com/tostmann/zigbee2mqtt) fork and are applied automatically via npm `postinstall`.  See [`patches/herdsman-ncp-reset-fix.md`](./patches/herdsman-ncp-reset-fix.md) in this repo for the technical rationale.

👉 **[Read the full Zigbee2MQTT Setup & Migration Guide](ZIGBEE2MQTT.md)**

## Home Assistant ZHA (experimental)

If you would rather drive the coordinator from Home Assistant's native ZHA
integration (i.e. avoid running Z2M at all), the
[`tostmann/zha-zboss-esp`](https://github.com/tostmann/zha-zboss-esp)
custom-component is the path. It is HACS-installable, bundles
`zigpy-zboss` as a Python requirement, runtime-patches the known library
compat gaps against current zigpy / serialx, and extends ZHA's
`RadioType` enum so **ZBOSS** appears as a selectable radio type in the
add-integration flow.

**Status as of v1.3.x**: the picture has improved substantially. The
`zigpy-zboss` maintainer is actively modernizing the library
([kardia-as/zigpy-zboss#73](https://github.com/kardia-as/zigpy-zboss/pull/73)
brings zigpy ≥ 0.92 / serialx support), and we have hardware-validated that
branch against this firmware end-to-end: `ControllerApplication.new(auto_form=True)`
on a factory-blank coordinator and restart-on-formed-network both succeed —
over USB and over the new UART transport. The v1.3.x firmware fixes that
made this work (full-spec `NWK_FORMATION` response, the NCP_RESET
freeze/semantics fixes) are in this release. **For production use, the Z2M
path above remains the recommended option** until the upstream library
release lands, after which `tostmann/zha-zboss-esp` will be re-pinned.

## Configuration Example (zigbee2mqtt)

Use the stable JTAG USB serial port in your `configuration.yaml` and configure the transmit power to fully utilize the ESP32-C6 amplifier:

```yaml
serial:
  port: /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
  adapter: zboss
advanced:
  transmit_power: 20
```

### UART instead of USB (since v1.3.x)

Wire the host's serial header to the coordinator — three wires, 3.3 V levels. Firmware default: the coordinator **transmits on GPIO22** and **receives on GPIO23**:

| Host (OpenWRT / Raspberry Pi header UART / USB-UART adapter) | ESP32-C6 default pin | XIAO ESP32-C6 silk |
|---|---|---|
| Host **RX** ← | **GPIO22** (coordinator TX) | D4 |
| Host **TX** → | **GPIO23** (coordinator RX) | D5 |
| GND | GND | GND |

If you get no response, the two data lines are the usual suspect — swap them. Quick check: the coordinator emits a 7-byte boot frame `DE AD 05 00 06 01 8F` on its TX line a moment after every reset.

```yaml
serial:
  port: /dev/ttyAMA0        # or /dev/ttyUSBx for a USB-UART adapter, or ser2net → tcp://...
  adapter: zboss
  baudrate: 115200
```

Pins and baud rate are build-time configurable (`menuconfig` → *Zigbee Network Co-processor*). USB-Serial/JTAG stays fully functional in parallel — flashing and recovery keep working over USB while the NCP link runs on the UART.


## Flashing Instructions (Web Installer & CLI)

### 1. Web Installer (Recommended)
You can flash the firmware directly from your browser using our Web Serial Flasher tool. This is the easiest method and requires no software installation.

👉 **[Launch ESP-Coordinator Web Flasher](https://install.busware.de/zboss/)** 

*(Supported Browsers: Chrome, Edge, Opera. The flasher auto-detects your chip and serves the matching image for **ESP32-C6** and **ESP32-C5**.)*

> 🧪 **Experimental — want the coordinator on Wi-Fi instead of USB?** Try the **[native WiFi-Coex flasher](https://install.busware.de/zboss/coex/)**: the ESP32-C6 joins your Wi-Fi itself and serves the NCP over TCP (no USB host, no ser2net/socat). It's a testground build — see [Experimental: native Wi-Fi (coexistence)](#-experimental-native-wi-fi-coexistence-mode-b) below, and please report back on [Discussion #1](https://github.com/tostmann/esp-coordinator/discussions/1).

### 2. Manual CLI Flashing
Alternatively, you can flash the provided factory binary directly to the `0x0` offset of your board. The single binary includes the bootloader, partition table, OTA data, and the app. **Use the binary that matches your chip** — they are not interchangeable (the C5's second-stage bootloader sits at flash `0x2000`, the C6's at `0x0`; each factory image already carries the correct internal layout and is written from `0x0`).

```bash
# ESP32-C6
esptool.py -p /dev/ttyACM0 --chip esp32c6 write_flash 0x0 binaries/factory.bin

# ESP32-C5
esptool.py -p /dev/ttyACM0 --chip esp32c5 write_flash 0x0 binaries/factory-c5.bin
```

### 3. Build from Source
You can compile the firmware yourself using the standard Espressif IoT Development Framework (ESP-IDF v5.5+):

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash
```