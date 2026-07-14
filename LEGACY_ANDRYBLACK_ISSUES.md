# Status of Reports on the Archived `andryblack/esp-coordinator`

`andryblack/esp-coordinator` was the original publication of this firmware. As
of mid-2026 that repository is **archived and read-only** — every issue is
locked and no new comments can be posted there. Its README points at this
active fork, [`tostmann/esp-coordinator`](https://github.com/tostmann/esp-coordinator),
which now ships releases, picks up issues, and tracks the backlog.

This document mirrors the answers that would otherwise have been posted to
each archived report, so users searching for those symptoms via Google or
following an in-issue cross-reference can find the current status.

If you have a fresh report of any of the symptoms below, please open a new
issue on [tostmann/esp-coordinator/issues](https://github.com/tostmann/esp-coordinator/issues)
— not on the archived repo.

## Resolved in [`v1.1.22`](https://github.com/tostmann/esp-coordinator/releases/tag/v1.1.22)

The web flasher at [install.busware.de/zboss](https://install.busware.de/zboss/)
serves the corresponding factory binary. CLI users: `binaries/factory.bin`
flashed at offset `0x0` with `esptool.py write_flash 0x0 binaries/factory.bin`.

### [andryblack#7](https://github.com/andryblack/esp-coordinator/issues/7) — Tuya device interview fails

Reporter saw `Failed to send SIMPLE_DESCRIPTOR_REQUEST` with a Tuya device
(IEEE `0x70b3d52b6011344d`, OUI prefix `70b3d5…`). CC2652 was unaffected.

**Cause:** the prebuilt ZBOSS stack does a strict length check on ZDP
`Simple_Desc_rsp` and silently drops responses from those devices that carry
trailing bytes — see [esp-zigbee-sdk#485](https://github.com/espressif/esp-zigbee-sdk/issues/485).
Z2M sees the request go out, no response arrives, the timeout-wrapped wait
surfaces as a send failure. CC2652 does the parse in its own stack and is
unaffected.

**Fix:** the firmware intercepts cluster `0x8004` indications, tolerantly
re-parses them in userspace, and synthesises a clean response for the host
(`try_intercept_simple_desc_rsp` in
[`main/zb_ncp.cpp`](https://github.com/tostmann/esp-coordinator/blob/master/main/zb_ncp.cpp),
commit `8a05931`). First known userspace workaround of #485, verified live on
`_TZ3000_w0qqde0g` (TS011F).

### [andryblack#11](https://github.com/andryblack/esp-coordinator/issues/11) — Missing NCP reset response after firmware startup

The fix sketched in the original report is essentially what shipped:
`app::start_int` now sends the two boot-time frames (ACK + NCP_RESET response
with the `tsn=0xFF` sentinel) right after `transport::start()`. See
[`main/app.cpp`](https://github.com/tostmann/esp-coordinator/blob/master/main/app.cpp).

### [andryblack#15](https://github.com/andryblack/esp-coordinator/issues/15) — Need to set Manufacturer code for Lumi devices

`status 31` from the stock firmware is `ESP_ERR_NOT_SUPPORTED` from the
prebuilt ZBOSS lib, which is why a userspace handler was needed.

The workaround has two halves and both are needed:

- **Firmware:** v1.1.22 implements `ZDO_SET_NODE_DESC_MANUF_CODE (0x0216)`.
- **Host:** the Z2M adapter has to actually call it, which is what
  [`tostmann/zigbee2mqtt`](https://github.com/tostmann/zigbee2mqtt) does via
  the `ghcr.io/tostmann/zigbee2mqtt-esp32:latest` Docker image. Setup steps in
  [ZIGBEE2MQTT.md](ZIGBEE2MQTT.md).

Paired Lumi devices interview cleanly once both sides are in place.

### [andryblack#19](https://github.com/andryblack/esp-coordinator/issues/19) — Controller starts in reset mode after power failure, devices lost

This is the cold-boot panID/channel race.

**Cause:** `zboss_start_no_autostart()` only registers a deferred callback in
the ZBOSS scheduler — it does not create a task on its own. Without the ZBOSS
main loop running, that callback never fires, so the persisted network info
(`panID`, `extendedPanID`, `channel`) is not loaded from NVRAM in time for
Z2M's first `getNetworkInfo()` query. Z2M sees `joined: false`, decides the
network does not match its `configuration.yaml`, and calls
`reset(FactoryReset)` — wiping every paired device.

**Fix:** drive the ZBOSS dispatch task at every boot from `app::start_int`
after `transport::start()`. Live-verified on a cold-booted coordinator: Z2M's
first `getNetworkInfo()` returns the persisted `panID`, `extPanID`, `channel`
from NVRAM.

Cross-references: [`andryblack#5`](https://github.com/andryblack/esp-coordinator/issues/5)
(same root cause, also archived) and
[`zigbee2mqtt#26152`](https://github.com/Koenkk/zigbee2mqtt/issues/26152).

## Status / context reports

### [andryblack#12](https://github.com/andryblack/esp-coordinator/issues/12) — Update ZBOSS / verify with latest Z2M

Active upstream now lives at
[`tostmann/esp-coordinator`](https://github.com/tostmann/esp-coordinator). It
ships against current `espressif/esp-zigbee-sdk` and is verified with the
current `zigbee-herdsman` `zboss` adapter on Z2M `latest` and `dev`. State as
of 2026-05:

- ESP-IDF v5.5.2, `esp-zigbee-lib` / `esp-zboss-lib` `^1.6.0`
- 200-node coordinator, +20 dBm TX, NVRAM persistence across cold boot
- Custom commands `0x0099 GET_NETWORK_BACKUP` / `0x009A RESTORE_NETWORK` for
  full raw-NVRAM transfer (used by `ghcr.io/tostmann/zigbee2mqtt-esp32`)
- Tuya `Simple_Desc_rsp` userspace intercept (see andryblack#7 above)

### [andryblack#13](https://github.com/andryblack/esp-coordinator/issues/13) — diepeterpan's fork with stability fixes

We audited [`diepeterpan/esp-coordinator`](https://github.com/diepeterpan/esp-coordinator)
HEAD (`7ddf3b3`, Aug 2025) item-by-item against the current upstream. Every
substantive fix is already present in v1.1.22, in some cases at a different
placement: cold-boot race, `ext_pan_id` endian reversal in
`GET_EXTENDED_PAN_ID`, `app_device_version = 1`, `GET_ZIGBEE_CHANNEL_MASK`
implementation, console-logging silenced over the NCP transport
(`CONFIG_LOG_DEFAULT_LEVEL_NONE=y`), ESP-IDF v5.5. The one divergence that
matters in the other direction: `esp-zboss-lib` is `^1.6.0` (locked at 1.6.4)
rather than pinned to 1.5.1 — the 1.5.1 pin was a workaround for a build
issue that Espressif has since fixed.

On top of those, v1.1.22 has audit fixes that diepeterpan's branch does not:
the Tuya intercept, APSDE indication buffer raised to 512 bytes for OTA /
multi-attribute reads, ZDO request-slot lifecycle hardening, 200-node
`max_children` — see `git log` between `e77c32f..e4263ec`.

### [andryblack#2](https://github.com/andryblack/esp-coordinator/issues/2) — Home Assistant ZHA / zigpy-zboss compatibility

This asked whether the firmware can be driven from Home Assistant's **ZHA**
integration (through the `zigpy-zboss` radio library) instead of only
Zigbee2MQTT. As of mid-2026 there is a working path, though it is younger and
less mature than the Z2M one.

- **Wire- and lifecycle-level compatibility is verified.** `zigpy-zboss`
  drives this firmware over USB-Serial/JTAG: `connect`, version, role /
  join-status getters, network formation, and persistence across a hardware
  reset all round-trip correctly against the ZBOSS NCP protocol the firmware
  exposes.
- **A companion HACS component wires it into ZHA:**
  [`tostmann/zha-zboss-esp`](https://github.com/tostmann/zha-zboss-esp) bundles
  `zigpy-zboss`, applies the runtime shims it currently needs against modern
  zigpy, and registers **ZBOSS** as a selectable radio type in ZHA's
  add-integration flow.
- **[v0.2.0](https://github.com/tostmann/zha-zboss-esp/releases/tag/v0.2.0)
  fixes a loading bug** ([`zha-zboss-esp#1`](https://github.com/tostmann/zha-zboss-esp/issues/1)):
  the 0.1.x component declared no config flow and had no other load trigger, so
  a plain HACS install only downloaded the files and never actually *ran* the
  component — the radio type was never registered and ZBOSS never appeared.
  v0.2.0 loads via a config entry, so the patch runs on every start and ZBOSS
  is offered in the radio picker.

**Still gated upstream:** full *device pairing* through ZHA depends on
`zigpy-zboss` fixes that are not all released yet
([kardia-as/zigpy-zboss#19](https://github.com/kardia-as/zigpy-zboss/issues/19),
PR #74). Until those land, **Zigbee2MQTT via
`ghcr.io/tostmann/zigbee2mqtt-esp32` remains the recommended, mature host
path** for this hardware. The ZHA route is usable for getting the coordinator
recognised and a network formed, but is still labelled early.

## Likely resolved — pending confirmation

### [andryblack#17](https://github.com/andryblack/esp-coordinator/issues/17) — Sensors don't auto-report (manual poll works)

**Likely fixed in [`v1.5.73`](https://github.com/tostmann/esp-coordinator/releases/tag/v1.5.73) — unconfirmed for the specific devices in this report.**

The originally-suspected cause (sleepy-end-device / PIM cadence) was wrong. The
real root cause surfaced while debugging
[tostmann/esp-coordinator#6](https://github.com/tostmann/esp-coordinator/issues/6):
the firmware's `GET_LOCAL_IEEE_ADDR` command returned the coordinator's own
64-bit IEEE address **with the bytes reversed**. The host (Zigbee2MQTT /
`zigbee-herdsman`) parses that field little-endian, so it stored a byte-reversed
*phantom* address for the coordinator and handed that phantom out as the bind
destination to every device that binds to the coordinator by IEEE address.

Battery-powered sensors are exactly the affected class. To send a *bound*
report a device first resolves the coordinator's IEEE with a broadcast
`NWK_addr_req`; it gets no answer (no device owns the phantom address) and so
never delivers the report — while a **manual read**, which addresses the
coordinator by its short address `0x0000` (always correct), works. That matches
the "manual poll works, auto-report doesn't" symptom precisely. In #6 the
phantom bind targets were written against the PowerCfg / battery cluster
(`0x0001`) among others, which is the reporting path these sensors use.

Two things worth noting:

- The bug was present since the firmware's very first release, so **every**
  affected install stored the phantom coordinator address.
- In #6, after flashing v1.5.73 and re-pairing, battery auto-reports from an
  IKEA blind (E2102) and an open/close remote (E1766) came back — the same
  reporting class as this report.

**Unconfirmed:** whether the specific Aqara / Mijia / Hue devices in *this*
report hung on this exact mechanism. Some of those vendors have their own
reporting quirks, and the original hardware is not available to us (this repo is
archived). It is the most likely explanation, not a proven one.

**What to do:** flash
[`v1.5.73`](https://github.com/tostmann/esp-coordinator/releases/tag/v1.5.73)
(web flasher at [install.busware.de/zboss](https://install.busware.de/zboss/)),
then **re-pair** the affected sensors — or run *Reconfigure* on them — so they
re-bind to the corrected coordinator address. Flashing alone is not enough: the
device keeps chasing the phantom address stored in its own binding table until
it is re-bound. If auto-reporting still fails afterwards, please open a new
issue on [tostmann/esp-coordinator/issues](https://github.com/tostmann/esp-coordinator/issues)
with the device model + vendor manuf-code so we can categorise.

## Not actionable / stale

The following reports are build / general help questions from 2024–
early-2025 that the original author or community members answered in-thread.
They do not represent open work for us:

- [andryblack#10](https://github.com/andryblack/esp-coordinator/issues/10) — compile question, 04/2025
- [andryblack#9](https://github.com/andryblack/esp-coordinator/issues/9) — generic "Help" question, 03/2025
- [andryblack#3](https://github.com/andryblack/esp-coordinator/issues/3) — documentation-tip from @Hedda
- [andryblack#1](https://github.com/andryblack/esp-coordinator/issues/1) — community offer to help with ZBOSS, 10/2024
