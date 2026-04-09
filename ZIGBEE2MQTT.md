# Zigbee2MQTT Integration & Custom Docker Image

The `busware.de ESP32 (ZBOSS)` coordinator firmware is fully compatible with the standard, official [Zigbee2MQTT](https://www.zigbee2mqtt.io) release. **You do not strictly need a custom version of Zigbee2MQTT to use this coordinator.**

However, we **highly recommend** using our customized Docker image.

## Why a Custom Docker Image?

The standard `zigbee-herdsman` library (which powers Zigbee2MQTT) currently lacks full native state transfer capabilities for the ZBOSS stack used on ESP32-C6/H2 chips. If your ESP32 hardware breaks and you plug in a new one, a standard Zigbee2MQTT setup cannot restore the old network state (Frame Counters, Trust Center Keys, etc.) to the new chip.

To solve this, we developed a powerful, chunk-based raw NVRAM memory transfer protocol (`0x0099` / `0x009A`) within this ESP32 firmware. 

Our custom Docker image automatically patches Zigbee2MQTT on startup to leverage this protocol. 

### Benefits of the Custom Image:
1. **Automated NVRAM Snapshots:** Zigbee2MQTT will automatically extract the complete 40KB Flash memory (NVS & `zb_storage`) of the ESP32 and save it as a Base64 string inside your standard `coordinator_backup.json`. This acts as a rolling snapshot of your network.
2. **Seamless Hardware Migration:** If your coordinator dies, simply flash our firmware onto a new ESP32-C6, plug it in, and start Zigbee2MQTT. It will detect the blank chip and automatically stream the latest memory snapshot back onto it. 
   *(**Note:** Because this is a raw memory image containing exact Frame Counters, the snapshot must be recent. If too many messages were sent after the snapshot was taken, end devices will reject the new coordinator due to Replay-Attack protection.)*
3. **Custom Branding:** The coordinator will proudly identify itself as `busware.de ESP32 (ZBOSS)` in the Zigbee2MQTT frontend and Home Assistant, rather than just a generic `zboss` adapter.

## How to Use It

Our GitHub Actions pipeline automatically builds multi-architecture Docker images (`amd64`, `arm64`, `arm/v7`) synchronized with the latest official Zigbee2MQTT releases.

Simply replace the official image in your `docker-compose.yml` with ours:

```yaml
version: '3.8'
services:
  zigbee2mqtt:
    container_name: zigbee2mqtt
    image: ghcr.io/tostmann/zigbee2mqtt-esp32:latest  # <-- Use our custom image
    restart: unless-stopped
    volumes:
      - ./data:/app/data
      - /run/udev:/run/udev:ro
    ports:
      - 8080:8080
    environment:
      - TZ=Europe/Berlin
    devices:
      # Replace with the actual path to your ESP32-C6 (e.g., /dev/ttyACM0 or by-id)
      - /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...:/dev/ttyACM0
```

### Home Assistant OS Users
For users running Home Assistant OS with the Add-on store, we plan to provide a custom Add-on repository in the future to allow 1-click installations of this patched version.

## Do I have to use this?

No. If you prefer to stick to the official `koenkk/zigbee2mqtt` image, everything will still work perfectly. Your devices will pair, route, and function normally. 

The only downside is that you won't have automated, hardware-agnostic snapshots. If your ESP32 breaks while using the standard image, you will either need to re-pair all your devices to the new coordinator or rely on manual CLI flash-dumping tools (like `esptool.py`) to extract and transfer the raw NVRAM before you swap the hardware.