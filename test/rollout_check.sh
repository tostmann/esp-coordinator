#!/bin/bash
# ==============================================================================
# ESP-COORDINATOR: ROLLOUT END-TO-END TEST
# ==============================================================================
# Dieses Skript flasht den Coordinator (NCP), baut & flasht einen Zigbee-Client
# (Router/End-Device) und triggert den Pairing-Prozess in Zigbee2MQTT, damit
# das Gerät in Home Assistant auftaucht und die LED per Dashboard geschaltet
# werden kann.
# ==============================================================================

set -e

# --- KONFIGURATION ---
COORD_PORT="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_54:32:04:03:59:68-if00"
DEVICE_PORT="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_58:E6:C5:E8:55:48-if00"
COORD_BIN="binaries/esp-coordinator-v1.1.0-esp32c6-factory.bin"
DEVICE_DIR="esp-zigbee-sdk/examples/esp_zigbee_HA_sample/HA_on_off_light"

echo "============================================================"
echo "🚀 STARTE END-TO-END ROLLOUT TEST: HASS -> Z2M -> NCP -> END DEVICE"
echo "============================================================"

# 1. Flashen des ESP32-C6 Coordinators
echo "[1/4] Flashe esp-coordinator v1.1.0 (NCP) auf $COORD_PORT..."
esptool.py -p $COORD_PORT -b 460800 --before default_reset --after hard_reset write_flash 0x0 $COORD_BIN
echo "✅ Coordinator erfolgreich geflasht."
sleep 2

# 2. Bauen & Flashen des Test-Zigbee-Geräts (TUL32-C6)
echo "[2/4] Baue & flashe Test-Endgerät (HA On/Off Light) auf $DEVICE_PORT..."
source ./idf_env.sh
cd $DEVICE_DIR
# Target setzen, falls noch nicht geschehen
idf.py set-target esp32c6
# Bauen und über den Device-Port flashen
idf.py -p $DEVICE_PORT build flash
cd - > /dev/null
echo "✅ Test-Gerät (Router) erfolgreich geflasht."

# 3. Zigbee2MQTT Neustart
echo "[3/4] Starte Zigbee2MQTT Service neu..."
sudo systemctl restart zigbee2mqtt
echo "Warte 15 Sekunden auf Z2M Initialisierung..."
sleep 15
echo "✅ Z2M online."

# 4. Anlern-Modus aktivieren (Permit Join)
echo "[4/4] Aktiviere Permit Join via MQTT..."
mosquitto_pub -h localhost -t "zigbee2mqtt/bridge/request/permit_join" -m '{"value": true}'
echo "✅ Anlernmodus aktiviert."

echo "============================================================"
echo "🎯 TEST-ABSCHLUSS & MANUELLE PRÜFUNG IN HASS"
echo "============================================================"
echo "Der Coordinator läuft und Z2M ist im Anlernmodus."
echo "Das Testgerät (TUL32-C6) sollte sich nun automatisch mit dem Netzwerk verbinden."
echo ""
echo "BITTE FOLGENDES MANUELL PRÜFEN:"
echo "1. Zigbee2MQTT Logs prüfen (Gerät 'Espressif' sollte als Router/EndDevice beitreten):"
echo "   tail -f zigbee2mqtt/data/log/$(ls -t zigbee2mqtt/data/log/ | head -n 1)/log.txt"
echo "2. Home Assistant öffnen (http://localhost:8123)."
echo "3. Unter 'Geräte & Dienste' -> 'MQTT' das neue Zigbee-Gerät suchen (z.B. 'Espressif Light')."
echo "4. Den Schalter für die Lampe im HASS-Dashboard betätigen."
echo "5. PRÜFUNG: Wenn HASS den Schalter umschaltet, muss die LED (GPIO8) am TUL32-C6 umschalten."
echo "============================================================"
