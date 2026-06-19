# WiFi-Coex (experimental) — native WiFi coordinator over TCP

> **Status: experimental testground build.** The ESP32-C6 has a single 2.4&nbsp;GHz
> radio that this firmware time-shares between WiFi and 802.15.4 Zigbee (software
> coexistence). Espressif rates the "WiFi-STA + always-on coordinator" case as
> supported-but-unstable. Whether it is good enough for a usable coordinator
> depends on your RF environment — that is what this build is here to help measure.
> Don't run a production network on it yet; use a spare stick and report back on
> [Discussion #1](https://github.com/tostmann/esp-coordinator/discussions/1).

The stable USB / UART coordinator is unchanged and lives at the normal flasher.
This page is only about the `wifi-coex` variant, where the coordinator joins your
WiFi itself and exposes the ZBOSS NCP on a raw TCP port — **no USB host, no
ser2net/socat bridge.**

## How it works (two modes)

The same image boots into one of two modes depending on whether it has WiFi
credentials stored:

- **Mode A — provisioning (no credentials yet).** Improv-Serial runs on
  USB-Serial/JTAG; the browser flasher prompts for your WiFi. The Zigbee stack
  does not start in this mode.
- **Mode B — operational (credentials present).** The device connects to WiFi
  and serves the NCP over TCP on port **6638**, alongside an optional UART link.
  Zigbee2MQTT connects with `port: tcp://<device-ip>:6638`.

## 1. Flash + connect to WiFi

Use the experimental flasher: **https://install.busware.de/zboss/coex/**

1. Connect the ESP32-C6 via USB, click the flash button, pick the serial port,
   flash.
2. ESP&nbsp;Web&nbsp;Tools then offers **"Connect to Wi-Fi"** — enter your WiFi
   SSID and password in the browser (Improv-Serial).
3. The device saves them and reboots onto your WiFi.

Browsers: Chrome / Edge / Opera (WebSerial). Safari and Firefox don't support it.

After provisioning, the browser flasher shows a **"Visit Device"** link
(`http://<device-ip>/`). It opens a small page **served by the coordinator
itself** that repeats these setup steps with this device's own address already
filled in — the quickest path to a working Z2M config.

## 2. Find the device on your network

- mDNS: `tcp://esp-zboss-coord.local:6638` (if your network resolves `.local`).
- Or use the IP from your router's DHCP client list: `tcp://<device-ip>:6638`.

A **static DHCP lease** is recommended so the address is stable.

## 3. Zigbee2MQTT configuration

The **ZBOSS adapter is mandatory**, and for this build the **matching Docker
image is required** — it ships the longer timeouts the single-radio TCP link
needs. Edit `zigbee2mqtt/data/configuration.yaml`:

```yaml
serial:
  port: tcp://esp-zboss-coord.local:6638   # mDNS name, or tcp://<device-ip>:6638
  adapter: zboss

advanced:
  transmit_power: 20
```

Docker image (required for the coex build):

```yaml
image: ghcr.io/tostmann/zigbee2mqtt-esp32:latest
```

The stock `koenkk/zigbee2mqtt` image uses serial-tuned timeouts that are too
short for the coexistence TCP link and can drop the connection under load.
See [ZIGBEE2MQTT.md](ZIGBEE2MQTT.md).

## 4. Re-configuring WiFi later (no reflash)

Changed your router or WiFi password? You don't need to erase or reflash:

1. Power-cycle the stick.
2. **Within the first 120 seconds after boot**, open the flasher again, click the
   button, pick the port, and choose **"Change Wi-Fi"**.
3. Enter the new credentials. The device saves them and reboots onto the new
   network.

The 120&nbsp;s Improv window reopens on **every** boot, so a wrong password is
always recoverable: power-cycle and try again. (The device validates the new
credentials on the next boot, not in the browser — if they're wrong it will keep
retrying / rebooting and reopen the window.)

## Caveats

- **Coexistence quality is RF-dependent and unproven.** A weak WiFi link makes it
  worse. Keep the device in good range of your AP, and prefer a Zigbee channel
  away from your WiFi channel.
- **Backup/restore over TCP is degraded.** The raw-NVRAM pull can stall on the
  coexistence link; the image falls back to a structured backup and a restore may
  be partial. For a full raw backup, flash the stable USB build temporarily and
  back up over USB.
- **Single host at a time.** One TCP client; running USB/serial and TCP hosts
  simultaneously is not supported.

## Please report back

This build exists to answer one question with real data: *is native WiFi
coexistence usable on the C6 as a coordinator?* Tell us what you saw on
[Discussion #1](https://github.com/tostmann/esp-coordinator/discussions/1):
your AP / environment, Zigbee + WiFi channels, device count, and how stable it
was under load.
