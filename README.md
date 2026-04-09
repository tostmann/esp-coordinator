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

3. **Native Bare-Metal Backups (New in v1.1.0)**
   Unlike Z-Stack or EZSP, which require hundreds of lines of complex parsing scripts to extract individual network keys and tables from RAM, this firmware implements a **Chunked Raw NVRAM Transfer**. Zigbee2MQTT can seamlessly read and restore the entire 40KB Flash memory (including frame counters, trust center keys, and routes) via standard NCP commands. You can swap a broken ESP32-C6 for a new one, hit restore, and the network will resume instantly.

4. **All-in-One SoC (Wi-Fi 6 + Zigbee)**
   Traditional network coordinators require two chips: a Zigbee radio (TI/SiLabs) and a Wi-Fi bridge (ESP32). The ESP32-C6 has both radios built into a single chip, drastically reducing hardware complexity for networked gateways.

5. **Modern ZBOSS Stack**
   The commercial, highly-certified ZBOSS stack provides an extremely robust alternative to the often complex and legacy-burdened EZSP protocol.


## Recent Fixes & Improvements (v1.1.0)

This project is an actively maintained, heavily optimized evolution of the original esp-coordinator.

* **Native NVRAM Backup & Restore:** Complete firmware-side implementation of custom NCP Commands `0x0099` and `0x009A`, supporting chunked Z2M backups.
* **NVRAM Persistence Fix:** Coordinator accurately resumes its network from NVRAM on boot, maintaining pairings and frame counters across restarts.
* **Synchronous NCP Reset Fix:** Reset acknowledgments (ACKs) are accurately sent back to the host *before* reboot, preventing Zigbee2MQTT handshake timeouts.
* **Manufacturer Code Workaround:** `ZDO_SET_NODE_DESC_MANUF_CODE` implementation allows Z2M to emulate the manufacturer code dynamically.
* **Network Scaling:** Tables and memory dynamically optimized for 200 nodes.
* **Dynamic TX Power:** Supports dynamic adjustment of the transmission power up to 20 dBm.
* **Permit Join Handling:** Added full support for the `NWK_PERMIT_JOINING (0x0404)` command.
* **Modern ZBOSS SDK:** Fully ported to ZBOSS SDK v1.6.x.


## Configuration Example (zigbee2mqtt)

Use the stable JTAG USB serial port in your `configuration.yaml` and configure the transmit power to fully utilize the ESP32-C6 amplifier:

```yaml
serial:
  port: /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
  adapter: zboss
advanced:
  transmit_power: 20
```


## Flashing Instructions (Web Installer & CLI)

### 1. Web Installer (Recommended)
You can flash the firmware directly from your browser using our Web Serial Flasher tool. This is the easiest method and requires no software installation.

👉 **[Launch ESP-Coordinator Web Flasher](https://install.busware.de/zboss/)** 

*(Supported Browsers: Chrome, Edge, Opera)*

### 2. Manual CLI Flashing
Alternatively, you can flash the provided factory binary directly to the `0x0` offset of your ESP32-C6. This single binary includes the bootloader, partition table, and the app.

```bash
esptool.py -p /dev/ttyACM0 --chip esp32c6 write_flash 0x0 binaries/esp-coordinator-v1.1.0-esp32c6-factory.bin
```

### 3. Build from Source
You can compile the firmware yourself using the standard Espressif IoT Development Framework (ESP-IDF v5.5):

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash
```