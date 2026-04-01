# esp-coordinator

ZBOSS NCP Serial Protocol implementation for ESP32-C6/H2 modules.
This implementation is focused on providing a stable Zigbee Coordinator role for usage with [Zigbee2MQTT](https://www.zigbee2mqtt.io).

[Protocol specification](https://wiki.homed.dev/files/9/95/ZBOSS_NCP_Serial_Protocol.pdf)

## Recent Fixes & Improvements

This fork/version incorporates critical bugfixes for a stable Zigbee2MQTT experience:
* **NVRAM Persistence Fix:** Coordinator accurately resumes its network from NVRAM on boot instead of being flagged as reset, maintaining pairings across restarts.
* **Synchronous NCP Reset Fix:** Reset acknowledgments (ACKs) are accurately sent back to the host *before* reboot, preventing Zigbee2MQTT handshake timeouts.
* **Manufacturer Code Workaround:** `ZDO_SET_NODE_DESC_MANUF_CODE` implementation allows Z2M to emulate the manufacturer code (e.g., Xiaomi/Aqara) dynamically, greatly improving sleepy end device pairing compatibility.
* **4MB Flash Support & Serial Logging:** Optimized configuration for 4MB flash targets and clean serial JTAG USB communication (no ESP-IDF system log interference).

## Configuration Example (zigbee2mqtt)

Use the stable JTAG USB serial port in your `configuration.yaml`.

```yaml
serial:
  port: /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
  adapter: zboss
```

## Flashing Instructions

You can flash the provided `factory.bin` directly to the `0x0` offset of your ESP32-C6. This single binary includes the bootloader, partition table, and the app.

```bash
esptool.py -p /dev/ttyACM0 --chip esp32c6 write_flash 0x0 binaries/factory.bin
```

Alternatively, you can build from source using the standard ESP-IDF (v5.5):
```bash
idf.py build
idf.py -p /dev/ttyACM0 flash
```

*Note: The project locks the `espressif/esp-zboss-lib` to version `1.5.1` due to compatibility constraints with the OSIF layer in newer SDK versions.*