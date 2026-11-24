"""The esp-coordinator ZBOSS NCP as an ESPHome component.

The firmware in ../../../main is built as an ESP-IDF component
(``zboss_ncp_idf/``, CONFIG_NCP_ESPHOME); ESPHome brings the network (WiFi,
Ethernet, WireGuard), OTA, logging and the native API. Zigbee2MQTT
(``adapter: zboss``) connects to the NCP over TCP.
"""

from pathlib import Path

from esphome import pins
import esphome.codegen as cg
from esphome.components import binary_sensor, select, sensor, socket, text_sensor
from esphome.components.esp32 import (
    VARIANT_ESP32C6,
    add_idf_component,
    add_idf_sdkconfig_option,
    add_partition,
    include_builtin_idf_component,
    only_on_variant,
    require_mbedtls_ecp,
    require_mbedtls_tls_extras,
    require_vfs_select,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_AP,
    CONF_CHANNEL,
    CONF_ID,
    CONF_PORT,
    CONF_RX_PIN,
    CONF_TX_PIN,
    CONF_UART_ID,
    CONF_WIFI,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)
from esphome.core import CORE, CoroPriority, coroutine_with_priority
import esphome.final_validate as fv

# No hard dependency on "network": with 'uart:' the host link is the UART and
# the device may run without WiFi or Ethernet (_final_validate checks it).
DEPENDENCIES = []
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "select"]
CONFLICTS_WITH = ["zigbee", "openthread"]

CONF_ALLOWED_IPS = "allowed_ips"
CONF_UART = "uart"
CONF_BAUD_RATE = "baud_rate"
CONF_EXTERNAL_ANTENNA = "external_antenna"
CONF_ANTENNA = "antenna"
ANTENNA_OPTIONS = ["On-board", "External (U.FL)"]
CONF_PAN_ID = "pan_id"
CONF_EXT_PAN_ID = "extended_pan_id"
CONF_HOST_CONNECTED = "host_connected"
CONF_NETWORK_LOST = "network_lost"
CONF_SAFE_MODE = "safe_mode"
CONF_FIRMWARE_VERSION = "firmware_version"

zboss_ncp_ns = cg.esphome_ns.namespace("zboss_ncp")
ZbossNcpComponent = zboss_ncp_ns.class_(
    "ZbossNcpComponent", cg.PollingComponent
)
AntennaSelect = zboss_ncp_ns.class_(
    "AntennaSelect", select.Select, cg.Parented.template(ZbossNcpComponent)
)

NCP_IDF_PATH = Path(__file__).parent / "zboss_ncp_idf"

UART_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_BAUD_RATE, default=115200): cv.positive_int,
    }
)

def _antenna_exclusive(config):
    if CONF_ANTENNA in config and config[CONF_EXTERNAL_ANTENNA]:
        raise cv.Invalid(
            f"'{CONF_EXTERNAL_ANTENNA}' and '{CONF_ANTENNA}' cannot be used together: "
            "the antenna select takes over the switch at runtime"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ZbossNcpComponent),
            cv.Optional(CONF_PORT, default=6638): cv.port,
            # Only these hosts/networks may open the NCP port (e.g. the
            # WireGuard peer). Empty = anyone who reaches the port.
            cv.Optional(CONF_ALLOWED_IPS, default=[]): cv.ensure_list(
                cv.ipv4network
            ),
            # Extra NCP host link on UART1, next to TCP (e.g. a Pi header).
            # The firmware takes UART1 itself, outside ESPHome's uart: buses.
            cv.Optional(CONF_UART): UART_SCHEMA,
            # Seeed XIAO ESP32-C6: external U.FL antenna (GPIO3/GPIO14, which
            # are untouched unless this or antenna: is set).
            cv.Optional(CONF_EXTERNAL_ANTENNA, default=False): cv.boolean,
            # Same switch, changeable at runtime and saved (Seeed XIAO only).
            # With WiFi it falls back to the on-board antenna when there is
            # no connection within 2 minutes on the external one.
            cv.Optional(CONF_ANTENNA): select.select_schema(
                AntennaSelect,
                icon="mdi:antenna",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_CHANNEL): sensor.sensor_schema(
                accuracy_decimals=0,
                icon="mdi:zigbee",
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PAN_ID): text_sensor.text_sensor_schema(
                icon="mdi:zigbee", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_EXT_PAN_ID): text_sensor.text_sensor_schema(
                icon="mdi:zigbee", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_FIRMWARE_VERSION): text_sensor.text_sensor_schema(
                icon="mdi:chip", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_HOST_CONNECTED): binary_sensor.binary_sensor_schema(
                device_class="connectivity",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_NETWORK_LOST): binary_sensor.binary_sensor_schema(
                device_class="problem", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_SAFE_MODE): binary_sensor.binary_sensor_schema(
                device_class="problem", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
        }
    ).extend(cv.polling_component_schema("10s")),
    cv.only_on_esp32,
    _antenna_exclusive,
    # wifi-coex is ESP32-C6-only (see CMakeLists.txt); the C5 is untested here.
    only_on_variant(supported=[VARIANT_ESP32C6]),
    # The NCP's TCP server: one listener, the active host and a pending one
    # (a reconnecting host is accepted next to a half-open old session).
    # ESPHome sizes lwIP's socket pool from these registrations.
    socket.consume_sockets(2, "zboss_ncp"),
    socket.consume_sockets(1, "zboss_ncp", socket.SocketType.TCP_LISTEN),
)


def _check_uart1(config, full):
    """The NCP's uart: link takes UART1 directly (main/transport.cpp). ESPHome
    numbers its own uart: buses UART0, UART1, ... and skips the logger's
    hardware UART, so it must not reach UART1."""
    if CONF_UART not in config:
        return
    logger = full.get("logger") or {}
    hw = logger.get("hardware_uart")
    logger_hw = hw in ("UART0", "UART1") and logger.get("baud_rate", 115200) != 0
    if logger_hw and hw == "UART1":
        raise cv.Invalid(
            "zboss_ncp: 'uart:' takes UART1, which the logger uses (hardware_uart: UART1)"
        )
    buses = len(full.get(CONF_UART, []) or [])
    if buses + (1 if logger_hw else 0) >= 2:
        raise cv.Invalid(
            "zboss_ncp: 'uart:' takes UART1 for the NCP, so ESPHome has only UART0 "
            "left for its own uart: bus (and the logger, if it uses a hardware UART)"
        )


def _final_validate(config):
    full = fv.full_config.get()
    _check_uart1(config, full)
    if "network" not in full and CONF_UART not in config:
        raise cv.Invalid(
            "zboss_ncp: no host link. Add a network (wifi: or ethernet:) for the "
            "TCP port, or 'uart:' to run the NCP over UART1 without a network."
        )
    if CONF_WIFI in full:
        if CONF_AP in full[CONF_WIFI]:
            raise cv.Invalid(
                "zboss_ncp: a WiFi access point (wifi: ap:) cannot share the radio "
                "with the Zigbee coordinator. Remove 'ap:' (and captive_portal)."
            )
        from esphome.components.wifi import force_power_save_off

        force_power_save_off(
            "zboss_ncp: WiFi power save starves the shared 802.15.4 radio"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


@coroutine_with_priority(CoroPriority.WORKAROUNDS)
async def _late_sdkconfig(config):
    # Late, so these win over the esp32 component's defaults.
    add_idf_sdkconfig_option("CONFIG_ZB_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_ZB_ZCZR", True)
    add_idf_sdkconfig_option("CONFIG_ZB_RADIO_NATIVE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE", 4096)
    # The prebuilt esp-zigbee-lib 1.6 port layer reads esp_log_default_level,
    # which only exists with dynamic log levels (ESPHome turns them off).
    add_idf_sdkconfig_option("CONFIG_LOG_DYNAMIC_LEVEL_CONTROL", True)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    add_idf_component(name="zboss_ncp_idf", path=str(NCP_IDF_PATH))
    add_idf_sdkconfig_option("CONFIG_NCP_ESPHOME", True)
    # Zigbee crypto (install codes, APS) inside the prebuilt libraries.
    require_mbedtls_ecp()
    require_mbedtls_tls_extras(
        ("CONFIG_MBEDTLS_CCM_C", "CONFIG_MBEDTLS_ECDSA_DETERMINISTIC")
    )
    require_vfs_select()

    # NCP settings (main/Kconfig.projbuild)
    add_idf_sdkconfig_option("CONFIG_NCP_TCP_PORT", config[CONF_PORT])
    add_idf_sdkconfig_option("CONFIG_NCP_USB_TRANSPORT", False)
    if uart := config.get(CONF_UART):
        add_idf_sdkconfig_option("CONFIG_NCP_UART_TRANSPORT", True)
        add_idf_sdkconfig_option("CONFIG_NCP_UART_TX_GPIO", uart[CONF_TX_PIN])
        add_idf_sdkconfig_option("CONFIG_NCP_UART_RX_GPIO", uart[CONF_RX_PIN])
        add_idf_sdkconfig_option("CONFIG_NCP_UART_BAUD", uart[CONF_BAUD_RATE])
    else:
        add_idf_sdkconfig_option("CONFIG_NCP_UART_TRANSPORT", False)
    add_idf_sdkconfig_option(
        "CONFIG_NCP_XIAO_EXT_ANTENNA", config[CONF_EXTERNAL_ANTENNA]
    )
    # The TCP server, its pending second client and ESPHome's own sockets.
    add_idf_sdkconfig_option("CONFIG_LWIP_SO_REUSE", True)

    use_wifi = CONF_WIFI in CORE.config
    cg.add(var.set_use_wifi(use_wifi))
    if use_wifi:
        include_builtin_idf_component("esp_coex")
        add_idf_sdkconfig_option("CONFIG_ESP_COEX_SW_COEXIST_ENABLE", True)
        # No post-connect roaming scans: ESPHome (2026.10.2) checks up to three
        # times per connection (the count restarts after a roam), five minutes
        # apart, and scans at -49 dBm or weaker; a scan takes the shared radio
        # off the Zigbee channel (and the driver warns about the non-default
        # scan times under coexistence). A coordinator does not move.
        wifi_var = await cg.get_variable(CORE.config[CONF_WIFI][CONF_ID])
        cg.add(wifi_var.set_post_connect_roaming(False))

    CORE.add_job(_late_sdkconfig, config)

    # Partitions the firmware expects, by name. zb_nvs is a placeholder for
    # the NVS half of a network backup (CONFIG_NCP_BACKUP_NVS_PARTITION): the
    # firmware never mounts it, "nvs" is ESPHome's and stays out of backups.
    # Same sizes as partitions.csv, so backups stay 40 KB and interchangeable
    # with the USB build.
    add_partition("zb_storage", "data", "fat", 0x4000)
    add_partition("zb_fct", "data", "fat", 0x1000)
    add_partition("zb_nvs", "data", "nvs", 0x6000)

    for net in config[CONF_ALLOWED_IPS]:
        cg.add(
            var.add_allowed_network(
                int(net.network_address), int(net.netmask)
            )
        )

    if conf := config.get(CONF_CHANNEL):
        cg.add(var.set_channel_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_PAN_ID):
        cg.add(var.set_pan_id_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_EXT_PAN_ID):
        cg.add(var.set_ext_pan_id_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_FIRMWARE_VERSION):
        cg.add(var.set_version_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_HOST_CONNECTED):
        cg.add(var.set_host_connected_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_NETWORK_LOST):
        cg.add(var.set_network_lost_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_ANTENNA):
        sel = await select.new_select(conf, options=ANTENNA_OPTIONS)
        await cg.register_parented(sel, var)
        cg.add(var.set_antenna_select(sel))
    if conf := config.get(CONF_SAFE_MODE):
        cg.add(var.set_safe_mode_sensor(await binary_sensor.new_binary_sensor(conf)))
