# Test-Dokumentation: ESP-COORDINATOR

Dieses Verzeichnis enthält Werkzeuge und Skripte zur Validierung der End-to-End-Funktionalität des ESP-COORDINATOR Systems. Der Fokus liegt auf der Sicherstellung, dass Coordinator, Zigbee-Geräte, Zigbee2MQTT und Home Assistant korrekt zusammenarbeiten.

## Inhaltsverzeichnis
1. [Übersicht: rollout_check.sh](#rollout_checksh)
2. [Voraussetzungen](#voraussetzungen)
3. [Konfiguration](#konfiguration)
4. [Durchführung des Tests](#durchführung-des-tests)
5. [Manueller Verifizierungsprozess](#manueller-verifizierungsprozess)

---

## rollout_check.sh

Das Skript `rollout_check.sh` ist ein automatisiertes Test-Tool für den vollständigen Rollout-Prozess. Es automatisiert die Bereitstellung der Firmware auf beiden Seiten des Zigbee-Netzwerks (Coordinator und Endgerät).

### Hauptfunktionen:
*   **Zigbee2MQTT Neustart:** Stoppt vorübergehend Zigbee2MQTT, um exklusiven Zugriff auf den seriellen Port zu gewährleisten.
*   **Coordinator-Flash:** Installiert die kompilierte NCP-Firmware (Network Co-Processor) auf dem ESP32-C6 Coordinator (via Factory-Image).
*   **Client-Build & Flash:** Kompiliert das offizielle `HA_on_off_light` Zigbee-Beispiel und flasht es auf ein zweites Test-ESP-Board (TUL32-C6).
*   **Z2M & Pairing:** Startet Zigbee2MQTT wieder, wartet auf die Initialisierung und sendet via MQTT den `permit_join` Befehl, um den Anlernmodus zu aktivieren.

---

## Voraussetzungen

Bevor das Skript ausgeführt wird, müssen folgende Bedingungen erfüllt sein:
*   **Hardware:** 
    *   1x ESP32-C6 (als Coordinator, verbunden z.B. an `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_54:32:04:03:59:68-if00`).
    *   1x ESP32-C6 (als Test-Client/Leuchte, verbunden z.B. an `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_58:E6:C5:E8:55:48-if00`).
*   **Software:**
    *   Installiertes ESP-IDF Framework v5.5.
    *   Lokale `mosquitto` Instanz (MQTT Broker).
    *   Lauffähige Instanz von Zigbee2MQTT als `systemd` Dienst (Name: `zigbee2mqtt`).

---

## Konfiguration

In der Datei `rollout_check.sh` sind die Hardware-Ports, die Coordinator-Firmware sowie das Test-Geräte-Projekt fest hinterlegt. Bei Änderungen der Hardware müssen die Ports (z.B. nach Austausch des Sticks) angepasst werden:

```bash
COORD_PORT="/dev/serial/by-id/usb-Espressif_..." # Port des Coordinators
DEVICE_PORT="/dev/serial/by-id/usb-Espressif_..." # Port des Test-Clients
COORD_BIN="binaries/esp-coordinator-v1.1.0-esp32c6-factory.bin"
DEVICE_DIR="esp-zigbee-sdk/examples/esp_zigbee_HA_sample/HA_on_off_light"
```

---

## Durchführung des Tests

Das Skript ist so konzipiert, dass es direkt aus dem Stammverzeichnis des Projekts aufgerufen wird.

```bash
chmod +x test/rollout_check.sh
./test/rollout_check.sh
```

Das Skript bricht bei Fehlern (`set -e`) sofort ab, um unklare Zustände zu vermeiden.

---

## Manueller Verifizierungsprozess

Nachdem das Skript erfolgreich durchgelaufen ist, sollte der Test-Client bereits dem Zigbee2MQTT-Netzwerk beigetreten sein (aufgrund des `permit_join` Befehls am Ende des Skripts). 

Zur finalen Validierung führen Sie folgende Schritte durch:

1.  **Zigbee2MQTT Logs prüfen:**
    Prüfen Sie, ob das Gerät dem Netzwerk beigetreten ist:
    ```bash
    tail -f zigbee2mqtt/data/log/$(ls -t zigbee2mqtt/data/log/ | head -n 1)/log.txt
    ```
    (Achten Sie auf Einträge wie `Device '0x...' joined`).

2.  **Test via MQTT (Simulierter Home Assistant Befehl):**
    Sie können den End-to-End-Fluss testen, indem Sie das Gerät per MQTT schalten:
    ```bash
    mosquitto_pub -h localhost -t "zigbee2mqtt/<IEEE-ADRESSE_DES_GERAETS>/set" -m '{"state": "ON"}'
    ```
    Die LED (Standard: GPIO8) am Testgerät sollte daraufhin aufleuchten.

3.  **Test in Home Assistant:**
    *   Navigieren Sie zu Home Assistant (z.B. `http://localhost:8123`).
    *   Suchen Sie unter *Geräte & Dienste -> MQTT* nach dem neuen "Espressif" Zigbee-Gerät.
    *   Schalten Sie die Lampe im HASS-Dashboard um und überprüfen Sie die physische LED am Test-Client.