# herdsman ZBOSS adapter — NCP_RESET unblock fix (DRAFT)

**Target fork:** `tostmann/zigbee2mqtt-esp32` (the `ghcr.io/tostmann/zigbee2mqtt-esp32` Docker image's bundled `zigbee-herdsman`).

**Status:** Draft. Not yet applied. Discussed with esp-coordinator firmware author 2026-05-20.

## What this fixes

`ZBOSSDriver.reset(...)` currently throws `Error: {commandId:2} after 10000ms` on
any factory-reset attempt (and likely on local-reset too) when the target NCP is
an ESP32-C6 running tostmann/esp-coordinator firmware. End-user symptom: z2m
exits with **"Failed to start zigbee-herdsman"** and systemd-restart-loops
whenever the `configuration.yaml` channel/panID doesn't match the device's
persisted network — i.e., whenever the user expects z2m to re-form the network.

## Root cause

Two layered behaviours combine:

1. `ZBOSSDriver.reset()` (driver.js:86–90) sets `port.inReset = true` *before*
   awaiting the NCP_RESET response:
   ```js
   async reset(options = enums_1.ResetOptions.NoOptions) {
       this.port.inReset = true;
       await this.execCommand(enums_1.CommandId.NCP_RESET, { options }, 10000);
   }
   ```

2. `ZBOSSUart.onPackage()` (uart.js:182–184) **unconditionally drops every
   inbound frame while inReset is true**:
   ```js
   async onPackage(data) {
       if (this.inReset)
           return;
       ...
   }
   ```

3. `inReset` is only cleared in `onPortClose()` (uart.js:171–177), after a 3s
   wait + reopen:
   ```js
   async onPortClose(err) {
       if (this.inReset) {
           await wait(3000);
           await this.openPort();
           this.inReset = false;
       }
   }
   ```

The intent in (3) is that the device reboots, USB drops, host sees port-close
→ wait 3s for re-enumeration → reopen → inReset clears. That works for an
EFR32/RCP setup with a UART bridge that genuinely drops the port on reset.

**It does not work on ESP32-C6 USB-Serial-JTAG**: the ROM bootloader
re-attaches the same USB CDC descriptor across `esp_restart()` essentially
instantly, so the host never raises a port-close event. `inReset` stays true,
`onPackage` keeps dropping frames, `execCommand` times out at 10s, reset() throws.

The firmware side already (a) sends the matching NCP_RESET response before any
blocking work and (b) does its best to force a real USB phy detach via
`USB_SERIAL_JTAG_CONF0_REG` (clear `DP_PULLUP` + `USB_PAD_ENABLE` for 800 ms)
before calling `esp_restart()` — see `main/commands_impl.h
ncp_reset_deferred_task`. The detach DOES make the host see a port close, which
triggers `onPortClose` → 3s wait → reopen → `inReset = false`. But:

- The very first NCP_RESET response (tsn matching the request) goes out before
  the detach and is dropped by `onPackage` while `inReset` is still true.
- The post-reboot boot-ready frame (`NCP_RESET response, tsn=0xFF` —
  unsolicited-boot sentinel) is sent by `continue_zboss` ~1–2s after
  `esp_restart()`, which is **inside the 3s wait window** while the host port
  is closed — so the frame is lost in the kernel CDC buffer / never delivered.

Net effect: the waiter is never resolved, even after `inReset` clears.

Note: the herdsman fork's `execCommand` (driver.js:235) already creates the
NCP_RESET waiter with `tsn: undefined` so any tsn matches — that part is
already correct, the boot-ready frame *would* satisfy the wait if it weren't
dropped.

## Fix — minimal change, single line

Remove the unconditional drop in `onPackage`. The CRC checks already filter
out garbage (ROM bootloader ASCII banners after esp_restart, electrical noise,
etc.). The only frames the drop "protected" us from were valid ZBOSS frames
that we sent ourselves around the reset boundary — which is exactly what we
need to *process*, not drop, to satisfy the pending `reset()` waiter.

### TypeScript source (preferred — for upstream PR)

```diff
--- a/src/adapter/zboss/uart.ts
+++ b/src/adapter/zboss/uart.ts
@@ -178,8 +178,18 @@ export class ZBOSSUart extends EventEmitter {
     }
 
     private async onPackage(data: Buffer): Promise<void> {
-        if (this.inReset) return;
+        // Intentionally do NOT drop frames while inReset.
+        //
+        // ESP32-C6 USB-Serial-JTAG re-attaches the same USB CDC descriptor
+        // across esp_restart() so the host doesn't necessarily see a
+        // port-close to trigger onPortClose() and clear inReset. Even when
+        // the firmware forces a USB phy detach, the post-reboot boot-ready
+        // frame can arrive *before* the 3s reopen wait completes — or the
+        // tsn-matching NCP_RESET response can arrive while inReset is still
+        // set. Dropping these turns reset() into an unconditional 10s timeout.
+        //
+        // The CRC checks below already reject any garbage (ROM banner,
+        // electrical noise, partial frames).
 
         const len = data.readUInt16LE(0);
         const pType = data.readUInt8(2);
```

### Compiled `dist/` (for hot-patch against running install)

```diff
--- a/dist/adapter/zboss/uart.js
+++ b/dist/adapter/zboss/uart.js
@@ -180,8 +180,7 @@ class ZBOSSUart extends node_events_1.default {
         logger_1.logger.info(`Port error: ${error}`, NS);
     }
     async onPackage(data) {
-        if (this.inReset)
-            return;
+        // inReset drop removed — see src/adapter/zboss/uart.ts for rationale.
         const len = data.readUInt16LE(0);
         const pType = data.readUInt8(2);
         const pFlags = data.readUInt8(3);
```

## What about onPortClose?

The 3s wait + reopen in `onPortClose` stays as-is. With the drop in `onPackage`
removed, the boot-ready frame (or the original NCP_RESET response) satisfies
the waiter on whichever side of the port-close cycle it arrives — either
during the brief window where `inReset` is still true (now processed instead
of dropped) or after `inReset` clears. Either way, `reset()` returns; the 10s
timeout never fires.

If the test confirms this works, the `inReset` flag itself can probably be
deleted entirely in a follow-up cleanup — but minimal-change wins for the
first PR.

## Risk assessment

The drop was almost certainly a defensive measure against frames arriving
during the "anything could happen, the chip's resetting" window. The risks of
removing it:

1. **A stale frame from the pre-reset session matches a new post-reset
   request.** Mitigated: ZBOSS NCP frames carry tsn, and herdsman's tsn
   counter does not reset across `reset()` (it's a per-driver-instance state).
   For NCP_RESET specifically, the waiter is already tsn-wildcard so any
   matching `commandId=2` response (pre- or post-reset) satisfies it.
2. **Garbage data parsed as a frame.** Mitigated: header CRC8 + body CRC16
   filter out anything that isn't a well-formed ZBOSS NCP frame.
3. **Process state confusion in `data_indication` handling.** Mitigated: the
   ZBOSS APSDE indication path is only active when the network is up — during
   reset the network is being torn down, so no APSDE frames should be
   generated. If any do arrive late, they're harmless to process (just
   forwarded as `INDICATION` to upper layers which can ignore them).

## Test plan

With the firmware-side `USB_SERIAL_JTAG_CONF0_REG` detach already in place
(see esp-coordinator `main/commands_impl.h ncp_reset_deferred_task`):

1. **Channel mismatch baseline (reproduces bug):** Set z2m
   `configuration.yaml` `channel:` to something different from the device's
   persisted channel. Start z2m. Confirm the `==> Error: Error: {"commandId":2} after 10000ms`
   timeout fires within 10s of `Driver reset` log line.
2. **Apply patch** to `dist/adapter/zboss/uart.js` (single-line drop removal).
3. **Restart z2m.** Confirm:
   - `Driver reset` log line appears.
   - Either a `<-- FRAME` log line with cmd=2 frame appears (matching the
     pending wait) shortly after, OR a `<<< FRAME` from the post-reboot boot
     window arrives and matches.
   - `Driver reset` is followed by `Driver startup` continuing normally (not
     by the 10s timeout).
   - z2m reaches `MQTT publish: 'zigbee2mqtt/bridge/state' payload '{"state":"online"}'`.
4. **Regression check:** also confirm the working case (channel-match, no
   factory reset triggered) still works.

## Files referenced

- `node_modules/zigbee-herdsman/dist/adapter/zboss/uart.js` (the actual
  compiled-and-running file on the user's z2m install)
- `src/adapter/zboss/uart.ts` (TypeScript source — for upstream PR if/when
  the source-side fork is identified; not present in the deployed Docker
  image)
- `node_modules/zigbee-herdsman/dist/adapter/zboss/driver.js` (no change
  needed — already handles tsn=undefined wildcard for NCP_RESET at line 235)
