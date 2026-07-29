#!/usr/bin/env python3
# Was ist das "status 254", das ein ZDO_PERMIT_JOINING_REQ an eine
# Broadcast-Adresse zurueckmeldet?
#
# Hypothese aus der Disassembly von libzboss_stack.zczr (esp-zboss-lib 1.6.4,
# zdo_nwk_manage_cli.c / zdo_common.c):
#
#   zdo_send_req()  prepends den ZDP-TSN VOR den Request-Body:
#       zb_buf_alloc_left(param,1); *ptr = tsn
#   Der Puffer enthaelt danach  [tsn][permit_duration][tc_significance].
#   Wird derselbe Puffer spaeter dem Request-Callback uebergeben (bei einem
#   Broadcast kommt NIE eine echte Mgmt_Permit_Joining_rsp), dann liest unser
#   req_cb ihn als zb_zdo_mgmt_permit_joining_resp_t{tsn,status} — und
#   "status" ist in Wahrheit das permit_duration-Byte.
#
#   z2m sendet duration=254 -> genau das beobachtete "status 254".
#   Zusatzbefund aus der Disassembly: duration==255 wird von ZBOSS auf 254
#   geklemmt (.L59: li s3,254), also muss Fall D ebenfalls 254 liefern.
#
# Vorhersage BESTAETIGT (7/7 on hardware): bei Broadcast folgte "status" der
# gesendeten duration, bei self/unicast 0x0000 blieb es 0. Seit dem Fix in
# cmd_handle<ZDO_PERMIT_JOINING_REQ>::response_body_is_synthetic() melden
# Broadcasts status 0; Fall G belegt, dass ein echter ZDP-Status auf dem
# Unicast-Pfad weiterhin unveraendert durchgereicht wird (0x85 TIMEOUT).
# Damit ist dieses Skript zugleich der Regressionstest fuer den Fix.
#
# Usage (Ziel ist Pflicht — entweder TCP-Host oder ein serieller Port):
#   python3 test/permit_bcast_status_probe.py <host> [port] [fixed]
#   python3 test/permit_bcast_status_probe.py /dev/ttyACMn      [fixed]
#
# Ohne "fixed" gilt das Echo-Modell als Erwartung (= Firmware VOR dem Fix).
# Mit "fixed" muss jeder Broadcast status 0 melden (= Firmware NACH dem Fix);
# damit ist dasselbe Skript das A/B auf derselben Hardware.
import sys, re, time, socket

ARGS = sys.argv[1:]
FIXED = "fixed" in ARGS
ARGS = [a for a in ARGS if a != "fixed"]
if not ARGS:
    sys.exit("Ziel fehlt: <host> [port] | /dev/ttyACMn   (optional: fixed)")
TARGET = ARGS[0]
PORT = int(ARGS[1]) if len(ARGS) > 1 else 2329
USE_SERIAL = TARGET.startswith("/dev/")

sys.argv = ["x", "/dev/null"]
src = re.sub(r'^main\(\)\s*$', '', open("test/permit_gate_check.py").read(), flags=re.M)
g = {"__name__": "pgc"}
exec(compile(src, "pgc", "exec"), g)
build_request = g["build_request"]
FrameReader = g["FrameReader"]

ZDO_PERMIT_JOINING_REQ = 0x020b


class SockSerial:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), 8)
        self.s.settimeout(0.2)
    def write(self, b): self.s.sendall(b)
    def read(self, n=4096):
        try:    return self.s.recv(n)
        except socket.timeout: return b""
    def reset_input_buffer(self):
        self.s.settimeout(0.05)
        try:
            while self.s.recv(4096): pass
        except Exception: pass
        self.s.settimeout(0.2)
    def close(self): self.s.close()


class SerialShim:
    """Gleiche Fassade wie SockSerial, aber auf einem echten Port.

    Geoeffnet wie in permit_gate_check.py: erst Attribute setzen, dtr/rts auf
    False, dann open(). Der Ein-Zeiler serial.Serial(dev,...) HAENGT am
    USB-Serial/JTAG des ESP32-C6 (hier reproduziert, Timeout ohne einen
    einzigen Frame).
    """
    def __init__(self, dev):
        import serial
        self.s = serial.Serial()
        self.s.port = dev; self.s.baudrate = 115200; self.s.timeout = 0.2
        self.s.dtr = False; self.s.rts = False
        self.s.open()
    def write(self, b): self.s.write(b)
    def read(self, n=4096): return self.s.read(n)
    def reset_input_buffer(self): self.s.reset_input_buffer()
    def close(self): self.s.close()


# (label, dst_nwk, duration, erwarteter status, Begruendung, Wartefenster s)
CASES = [
    ("A self 0x0000 dur=60",   0x0000,  60,   0, "echte lokale Antwort", 3.0),
    ("B bcast 0xfffc dur=60",  0xfffc,  60,  60, "Echo der duration", 3.0),
    ("C bcast 0xfffc dur=100", 0xfffc, 100, 100, "Echo der duration", 3.0),
    ("D bcast 0xfffc dur=255", 0xfffc, 255, 254, "ZBOSS klemmt 255 -> 254", 3.0),
    ("E bcast 0xfffc dur=0",   0xfffc,   0,   0, "Echo der duration (0)", 3.0),
    ("F bcast 0xffff dur=77",  0xffff,  77,  77, "Echo, andere Bcast-Adresse", 3.0),
    # Gegenprobe zum Fix: ein Unicast an ein Geraet, das es nicht gibt, MUSS
    # weiter einen echten Fehlerstatus liefern. want=-1 heisst "irgendetwas != 0".
    # Braucht ein langes Fenster: die Antwort kommt erst nach dem ZDO-Timeout
    # der Adressaufloesung, nicht in den ueblichen 3 s.
    ("G unicast 0x1234 dur=60", 0x1234, 60,  -1, "echter Fehler, darf nicht 0 werden", 70.0),
]


def send_and_read(ser, rd, dst, dur, tsn, seq, wait=3.0):
    payload = bytes([dst & 0xFF, (dst >> 8) & 0xFF, dur & 0xFF, 0x01])
    ser.write(build_request(ZDO_PERMIT_JOINING_REQ, payload=payload, tsn=tsn, seq=seq))
    got = []
    t0 = time.time()
    while time.time() - t0 < wait:
        for f in rd.poll(0.5):
            if len(f) < 16:
                continue                       # reines ACK
            p = f[9:]
            if p[1] != 1:
                continue                       # nur RESPONSE auswerten
            cmd = int.from_bytes(p[2:4], "little")
            if cmd != ZDO_PERMIT_JOINING_REQ:
                continue
            got.append((p[4], p[5], p[6]))     # tsn, category, status
        if got:
            break
    return got


where = TARGET if USE_SERIAL else f"{TARGET}:{PORT}"
print(f"== permit-join Broadcast-Status-Probe gegen {where} ==")
if FIXED:
    print("   Modus: NACH dem Fix — jeder Broadcast muss status 0 melden\n")
else:
    print("   Modus: VOR dem Fix — Broadcast-'status' == gesendete duration\n")
if USE_SERIAL:
    ser = SerialShim(TARGET)
    time.sleep(4.0)          # Port-Open pulst einen Reset: Boot-Frames abwarten
else:
    ser = SockSerial(TARGET, PORT)
    time.sleep(1.0)
ser.reset_input_buffer()
rd = FrameReader(ser)

results = []
for i, (label, dst, dur, want_raw, why, wait) in enumerate(CASES):
    want = 0 if (FIXED and dst >= 0xFFF8) else want_raw
    why = "Fix: Broadcast -> 0" if want != want_raw else why
    tsn = 0x30 + i
    got = send_and_read(ser, rd, dst, dur, tsn, (i + 1) & 3, wait)
    if not got:
        print(f"{label:26} -> KEINE Antwort in {wait:.0f} s   (erwartet {want}, {why})")
        results.append((label, None, want))
        continue
    rtsn, cat, status = got[0]
    ok = (status != 0 if want == -1 else status == want) and (rtsn == tsn)
    soll = "!=0" if want == -1 else str(want)
    print(f"{label:26} -> tsn={rtsn:#04x} cat={cat} status={status:<3} "
          f"erwartet {soll:<3} {'OK ' if ok else 'ABWEICHUNG'}  ({why})")
    results.append((label, status if not ok else want, want))
    time.sleep(0.5)

# Netz wieder zumachen (Fall A hatte es lokal fuer 60 s geoeffnet).
send_and_read(ser, rd, 0x0000, 0, 0x3f, 0)
ser.close()

hits = sum(1 for _, s, w in results if s == w)
print(f"\n== {hits}/{len(results)} Erwartungen getroffen ==")
if FIXED:
    print("PASS: Broadcast meldet status 0" if hits == len(results)
          else "FAIL: der Fix greift nicht wie erwartet")
elif hits == len(results):
    print("Modell bestaetigt: das Broadcast-'status'-Byte ist das echogte")
    print("permit_duration, kein ZDP-Status. Mapping auf 0 verdeckt keinen Fehler.")
else:
    print("Modell NICHT bestaetigt -> Ursache offen, NICHT patchen.")
