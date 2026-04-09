#!/bin/bash
# Backup & Restore Script für den ESP32-C6 Zigbee Coordinator

PORT="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_58:E6:C5:E8:55:48-if00"
BACKUP_FILE="coordinator_nvram_backup.bin"

echo "=== ESP32-C6 Zigbee Coordinator Backup Tool ==="
echo "Stelle sicher, dass Zigbee2MQTT gestoppt ist (z.B. systemctl stop zigbee2mqtt), bevor du fortfährst!"
echo "1) Backup erstellen (NVS & zb_storage speichern)"
echo "2) Backup wiederherstellen (NVS & zb_storage flashen)"
echo "3) Abbrechen"
read -p "Auswahl (1-3): " choice

case $choice in
    1)
        echo "Erstelle Backup des 4MB Flash-Speichers (sicherste Methode)..."
        python -m esptool --chip esp32c6 -p $PORT -b 460800 read_flash 0x0 0x400000 $BACKUP_FILE
        echo "✅ Backup erfolgreich unter '$BACKUP_FILE' gespeichert!"
        ;;
    2)
        if [ ! -f "$BACKUP_FILE" ]; then
            echo "❌ Fehler: Backup-Datei '$BACKUP_FILE' nicht gefunden!"
            exit 1
        fi
        echo "Stelle Backup wieder her..."
        python -m esptool --chip esp32c6 -p $PORT -b 460800 write_flash 0x0 $BACKUP_FILE
        echo "✅ Restore erfolgreich! Du kannst Zigbee2MQTT jetzt wieder starten."
        ;;
    3)
        echo "Abgebrochen."
        exit 0
        ;;
    *)
        echo "Ungültige Auswahl."
        exit 1
        ;;
esac
