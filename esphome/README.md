# ESPHome component: `zboss_ncp`

This directory builds the coordinator firmware in `../main` as an
[ESPHome](https://esphome.io) external component. ESPHome then takes care of
everything around the NCP: the network (WiFi, or Ethernet such as a W5500 over
SPI), provisioning, OTA, logging, WireGuard and a native API with diagnostic
sensors in Home Assistant. The host connects over TCP, exactly as with the
wifi-coex build:

- **Zigbee2MQTT:** `serial: { port: "tcp://<ip>:6638", adapter: zboss }`
  (see [ZIGBEE2MQTT.md](../ZIGBEE2MQTT.md) for the TCP timeouts)
- **ZHA:** radio type ZBOSS, `socket://<ip>:6638`

Supported chip: ESP32-C6 (like the wifi-coex build).

## Use

```yaml
esp32:
  variant: esp32c6
  flash_size: 4MB
  framework:
    type: esp-idf

external_components:
  - source:
      type: git
      url: https://github.com/tostmann/esp-coordinator
      ref: wifi-coex
      path: esphome/components
    components: [zboss_ncp]

zboss_ncp:
  port: 6638                 # NCP over TCP
  allowed_ips:               # optional: only these hosts may connect
    - 192.168.1.10/32        # e.g. the Zigbee2MQTT host, or a WireGuard network
  # uart:                    # optional extra host link on UART1
  #   tx_pin: GPIO22
  #   rx_pin: GPIO23
  # external_antenna: true   # Seeed XIAO ESP32-C6: U.FL antenna
  channel:          { name: Zigbee channel }
  pan_id:           { name: Zigbee PAN ID }
  extended_pan_id:  { name: Zigbee extended PAN ID }
  firmware_version: { name: NCP firmware }
  host_connected:   { name: Zigbee host connected }
  network_lost:     { name: Zigbee network lost }
  safe_mode:        { name: NCP safe mode }
```

Complete configurations: [examples/zigbee-coordinator.yaml](examples/zigbee-coordinator.yaml)
(WiFi + WireGuard) and [examples/zigbee-coordinator-ethernet.yaml](examples/zigbee-coordinator-ethernet.yaml)
(W5500 + WireGuard). Copy `examples/secrets.example.yaml` to `secrets.yaml`.

## How it is built

`components/zboss_ncp/zboss_ncp_idf/` is an ESP-IDF component that compiles
`../main/*.cpp` with `CONFIG_NCP_ESPHOME=y`. That option (in
`main/Kconfig.projbuild`, default off) changes only this build:

| | idf.py build | ESPHome build |
| --- | --- | --- |
| Network, provisioning | `wifi_coex.cpp`, Improv, info page | ESPHome (`wifi`, `ethernet`, `wireguard`, `improv_serial`, ...); `zboss_ncp_idf/src/ncp_host.cpp` answers the firmware's `wifi_coex_*` calls |
| Host link | USB-Serial/JTAG, UART1, TCP | TCP, optionally UART1 (`NCP_USB_TRANSPORT=n`: USB-Serial/JTAG belongs to ESPHome's logger) |
| NVS half of a network backup | `nvs` | `zb_nvs` (`NCP_BACKUP_NVS_PARTITION`; `nvs` is ESPHome's). Same 24 KB, so backups stay 40 KB and interchangeable |
| Start | `app_main()` | `zboss_ncp_main()` on a task started by the component (priority 1, as the main task); a failed start is logged instead of `abort()` |
| Who may connect | anyone who reaches the port | `allowed_ips` (`ncp_tcp_peer_allowed()`) |

The firmware's boot order is kept: the component sets up before ESPHome's WiFi
and waits until the Zigbee stack is up and coexistence is enabled
(`wifi_coex_start_connect()`, at `SKIP_STARTUP` or in safe mode) before WiFi
starts. Initialising WiFi and the 802.15.4 radio at the same time can deadlock
in `wifi_hw_start` (task watchdog); three such boots put the NCP in safe mode.
WiFi power save is forced off, and `wifi: ap:` is refused (an access point
cannot share the radio with the coordinator).

Partitions are added through ESPHome: `zb_storage` (16 KB), `zb_fct` (4 KB),
`zb_nvs` (24 KB). The component conflicts with ESPHome's own `zigbee` (end
device/router, esp-zigbee-lib 2.x) and `openthread`.

## Notes

- **WiFi and Zigbee share one radio on the C6** (see [WIFI_COEX.md](../WIFI_COEX.md)).
  Ethernet (W5500 or DM9051 over SPI) leaves the radio to Zigbee alone.
- The NCP protocol has no authentication: use `allowed_ips` and/or reach the
  port only through a WireGuard tunnel.
- `NCP_RESET` and a network restore reboot the whole device, as on the stick.
- The component turns `CONFIG_LOG_DYNAMIC_LEVEL_CONTROL` back on (ESPHome
  disables it): the prebuilt esp-zigbee-lib 1.6 port layer references
  `esp_log_default_level`.

## Tools

- `tools/migrate_stick.py`: move the Zigbee network of a C6 running the
  USB/wifi-coex firmware into this layout (`esptool read_flash` dump of the
  same chip in, `zb_storage`/`zb_nvs` images and their new offsets out). Same
  chip, same IEEE address: paired devices stay paired.
- `tools/ncp_probe.py <host>`: read-only check over TCP (version, joined,
  channel, PAN ids). `--reset` sends `NCP_RESET` option 0 (restart, network
  kept, boot guard cleared), the way out of safe mode. Stop the host first:
  the NCP serves one host at a time.

## Tested

ESPHome 2026.10.2 (ESP-IDF 5.5.6). ESP32-C6 over WiFi with Zigbee2MQTT
(`adapter: zboss`): network migrated from a stick running v1.5.81-coex,
devices switch, host reconnect and preemption. Builds for W5500 Ethernet.
Not yet tested on hardware: Ethernet, WireGuard, UART, ZHA, backup/restore
over TCP.
