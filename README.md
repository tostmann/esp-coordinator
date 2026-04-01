# esp-coordinator

ZBOSS NCP Serial Protocol implementation for ESP32-C6/H2 modules.
This implementation is focused on providing a stable Zigbee Coordinator role for usage with [Zigbee2MQTT](https://www.zigbee2mqtt.io).

[Protocol specification](https://wiki.homed.dev/files/9/95/ZBOSS_NCP_Serial_Protocol.pdf)

## Recent Fixes & Improvements (v1.1.0)

This project is an actively maintained, heavily optimized evolution of the original esp-coordinator. It is specifically tailored for a highly stable **Zigbee2MQTT** experience and serves as a standalone, production-ready firmware alternative to TI or SiLabs-based adapters for ESP32-C6/H2 modules.

### Key Features & Improvements (v1.1.0)
* **NVRAM Persistence Fix:** Coordinator accurately resumes its network from NVRAM on boot instead of being flagged as reset, maintaining pairings across restarts.
* **Synchronous NCP Reset Fix:** Reset acknowledgments (ACKs) are accurately sent back to the host *before* reboot, preventing Zigbee2MQTT handshake timeouts.
* **Manufacturer Code Workaround:** `ZDO_SET_NODE_DESC_MANUF_CODE` implementation allows Z2M to emulate the manufacturer code dynamically.
* **Network Scaling:** Tables optimized for 200 nodes.
* **Network Backup & Restore:** Firmware-side implementation of Custom-NCP-Commands `0x0099` (Backup) and `0x009A` (Restore).
* **Dynamic TX Power:** Supports dynamic adjustment of the transmission power.
* **Factory Reset Handling:** Correct processing of `NCP_RESET` from Z2M.
* **Permit Join Handling:** Added full support for the `NWK_PERMIT_JOINING (0x0404)` command.
* **Memory Leak Fix:** Fixed a memory leak in the Green Power handler.
* **Modern ZBOSS SDK:** Fully ported to ZBOSS SDK v1.6.x.

## Configuration Example (zigbee2mqtt)

Use the stable JTAG USB serial port in your `configuration.yaml`.

```yaml
serial:
  port: /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
  adapter: zboss
advanced:
  transmit_power: 20
```

## Flashing Instructions (Web Installer & CLI)

### 1. Web Installer (Recommended)
You can flash the `v1.1.0` firmware directly from your browser using our Web Serial Flasher tool. 

👉 **[Launch ESP-Coordinator Web Flasher](https://install.busware.de/zboss/)** 

(Supported Browsers: Chrome, Edge, Opera)

### 2. Manual CLI Flashing
Alternatively, you can flash the provided factory binary directly to the `0x0` offset of your ESP32-C6. This single binary includes the bootloader, partition table, and the app.

```bash
esptool.py -p /dev/ttyACM0 --chip esp32c6 write_flash 0x0 binaries/esp-coordinator-v1.1.0-esp32c6-factory.bin
```

Alternatively, you can build from source using the standard ESP-IDF (v5.5):
```bash
idf.py build
idf.py -p /dev/ttyACM0 flash
```
