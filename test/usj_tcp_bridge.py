#!/usr/bin/env python3
# usj_tcp_bridge.py — stellt einen ESP32-C6/H2-NCP mit nativem USB-Serial/JTAG
# als TCP-Port bereit (z.B. fuer `serial: port: tcp://127.0.0.1:2330` in z2m).
#
# WARUM: zigbee-herdsman (und jede Standard-serialport-Anbindung) oeffnet den
# Port mit ASSERTIERTEM DTR/RTS. Auf dem USB-Serial/JTAG ist genau diese
# Kombination die esptool-Auto-Reset-Sequenz — der Chip landet im
# ROM-Download-Mode und antwortet nie auf NCP-Frames. Symptom: z2m stirbt an
# `{"commandId":20} after 30000ms`, danach ist der Stick auch fuer eigene
# Skripte stumm (Write-Timeout), bis ein `esptool --after hard_reset` ihn
# wieder startet.
#
# Diese Bridge oeffnet seriell mit dtr=False/rts=False (die Semantik, mit der
# der NCP nachweislich antwortet) und reicht die Bytes 1:1 ueber TCP weiter.
#
# Usage: python3 test/usj_tcp_bridge.py <serial-dev> [port] [laufzeit-s]
import os
import socket
import sys
import threading
import time

import serial

DEV = os.path.realpath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 2330
RUNTIME = float(sys.argv[3]) if len(sys.argv) > 3 else 3600

ser = serial.Serial()
ser.port = DEV
ser.baudrate = 115200
ser.timeout = 0.05
ser.write_timeout = 5
ser.dtr = False
ser.rts = False
ser.open()
print(f"serial open: {DEV} (dtr/rts deassertiert)", flush=True)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(1)
srv.settimeout(5)
print(f"listening on 127.0.0.1:{PORT}", flush=True)

deadline = time.time() + RUNTIME
while time.time() < deadline:
    try:
        conn, addr = srv.accept()
    except socket.timeout:
        continue
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.settimeout(0.5)
    print(f"client connected: {addr}", flush=True)
    stop = threading.Event()

    def pump_serial_to_tcp(c=conn, ev=stop):
        while not ev.is_set():
            try:
                data = ser.read(4096)
            except Exception:
                ev.set()
                return
            if data:
                try:
                    c.sendall(data)
                except OSError:
                    ev.set()
                    return

    pump = threading.Thread(target=pump_serial_to_tcp, daemon=True)
    pump.start()
    while not stop.is_set():
        try:
            data = conn.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not data:
            break
        try:
            ser.write(data)
        except Exception as exc:
            print(f"serial write failed: {exc}", flush=True)
            break
    stop.set()
    pump.join(1)
    try:
        conn.close()
    except OSError:
        pass
    print("client disconnected", flush=True)

ser.close()
print("bridge beendet", flush=True)
