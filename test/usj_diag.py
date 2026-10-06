#!/usr/bin/env python3
"""Check what an ESP32-H2/C6 coordinator on its USB-Serial/JTAG port is doing,
without resetting it. Needs only Python 3 (Linux), no extra packages.

Stop Zigbee2MQTT first, then run it with the port path from your Zigbee2MQTT
configuration:

    python3 usj_diag.py /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_..-if00

The port is opened the way Zigbee2MQTT opens it: the kernel raises DTR and RTS
together and nothing is toggled afterwards, which does not reset the chip.

    python3 usj_diag.py --reset <port>

restarts the chip into its firmware instead, with the "reset SoC into booting
from flash" sequence of the ESP32-H2 TRM (table 33.4-2; RTS set while DTR is
clear resets the chip, table 33.3-2). Run the check first: the reset wipes out
the state the check is meant to see.
"""
import errno
import fcntl
import os
import select
import struct
import sys
import termios
import time

# GET_MODULE_VERSION request, ZBOSS NCP framing, tsn 0x31
NCP_REQUEST = bytes.fromhex("dead0c0006c07cd67a0000010031")
# GET_JOINED request, tsn 0x32 (the first command Zigbee2MQTT sends)
JOINED_REQUEST = bytes.fromhex("dead0c0006c48465f40000140032")
# ESP ROM bootloader SYNC command, SLIP framed (the first packet esptool sends)
ROM_SYNC = bytes([0xC0, 0x00, 0x08, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00,
                  0x07, 0x07, 0x12, 0x20]) + b"\x55" * 32 + b"\xC0"
# Start of the frame the coordinator firmware sends once at every start-up
STARTUP_FRAME = bytes.fromhex("dead0e0006c05d")


def open_port(path):
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as e:
        sys.exit("cannot open %s: %s" % (path, e.strerror))
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        sys.exit("%s is in use - stop Zigbee2MQTT first" % path)
    try:
        attrs = termios.tcgetattr(fd)
    except termios.error:
        os.close(fd)
        sys.exit("%s is not a serial port" % path)
    attrs[0] = 0                                    # no input translation, no XON/XOFF
    attrs[1] = 0                                    # raw output
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL
    attrs[3] = 0                                    # no echo, no line editing
    attrs[4] = attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)  # input is NOT flushed on purpose
    return fd


def modem_lines(fd):
    v = struct.unpack("i", fcntl.ioctl(fd, termios.TIOCMGET, struct.pack("i", 0)))[0]
    return "DTR=%d RTS=%d" % (bool(v & termios.TIOCM_DTR), bool(v & termios.TIOCM_RTS))


def read_for(fd, seconds):
    end = time.monotonic() + seconds
    data = bytearray()
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return bytes(data)
        if select.select([fd], [], [], left)[0]:
            try:
                data += os.read(fd, 4096)
            except OSError as e:
                if e.errno != errno.EAGAIN:
                    sys.exit("read failed: %s (device gone?)" % e.strerror)


def write_until(fd, data, deadline):
    """Write without blocking past the deadline; returns the bytes the kernel took."""
    sent = 0
    while sent < len(data):
        left = deadline - time.monotonic()
        if left <= 0 or not select.select([], [fd], [], left)[1]:
            break
        try:
            sent += os.write(fd, data[sent:])
        except OSError as e:
            if e.errno != errno.EAGAIN:
                break
    return sent


def out_queue(fd):
    return struct.unpack("i", fcntl.ioctl(fd, termios.TIOCOUTQ, struct.pack("i", 0)))[0]


def show(data, limit=160):
    text = data.decode("ascii", "replace")
    if data and sum(c.isprintable() or c in "\r\n" for c in text) > 0.9 * len(text):
        return "text: " + " | ".join(l for l in text.replace("\r", "").split("\n") if l)[:limit * 2]
    return data[:limit].hex()


def firmware_version(reply):
    i = reply.find(bytes.fromhex("0001010031"))  # response, GET_MODULE_VERSION, tsn 0x31
    if i < 0 or len(reply) < i + 11:
        return None
    if reply[i + 5:i + 7] != b"\x00\x00":
        return "status %02x/%02x" % (reply[i + 5], reply[i + 6])
    fw = struct.unpack("<I", reply[i + 7:i + 11])[0]
    return "firmware %d.%d.%d" % (fw >> 24, (fw >> 16) & 0xFF, fw & 0xFFFF)


def joined_state(reply):
    i = reply.find(bytes.fromhex("0001140032"))  # response, GET_JOINED, tsn 0x32
    if i < 0 or len(reply) < i + 7:
        return None
    if reply[i + 5:i + 7] == b"\x00\x02":
        return "GET_JOINED: BLOCKED - the firmware runs in safe mode"
    if reply[i + 5:i + 7] != b"\x00\x00" or len(reply) < i + 8:
        return "GET_JOINED: status %02x/%02x" % (reply[i + 5], reply[i + 6])
    return "GET_JOINED: joined=%d" % reply[i + 7]


def check(path):
    print("usj_diag: %s -> %s, Python %s" % (path, os.path.realpath(path), sys.version.split()[0]))
    fd = open_port(path)
    print("   opened, control lines as set by the kernel: %s" % modem_lines(fd))

    queued = read_for(fd, 1.0)
    print("1. bytes the chip had queued or sent by itself: %d%s"
          % (len(queued), (" -> " + show(queued)) if queued else ""))
    if STARTUP_FRAME in queued:
        print("   contains the firmware's start-up frame: the chip restarted after the host"
              " enumerated it, and nothing has read the port since")

    write_until(fd, NCP_REQUEST, time.monotonic() + 1.0)
    reply = read_for(fd, 2.0)
    write_until(fd, JOINED_REQUEST, time.monotonic() + 1.0)
    reply += read_for(fd, 1.5)
    found = [x for x in (firmware_version(reply), joined_state(reply)) if x]
    print("2. coordinator firmware answered: %s%s"
          % ("YES (%s)" % "; ".join(found) if found else ("no" if not reply else "unclear"),
             (" -> " + show(reply, 64)) if reply and not found else ""))

    write_until(fd, ROM_SYNC, time.monotonic() + 1.0)
    reply = read_for(fd, 1.5)
    rom = b"\xc0\x01\x08" in reply
    print("3. ESP ROM bootloader answered: %s" % ("YES - the chip is in download mode" if rom else "no"))

    start = time.monotonic()
    deadline = start + 3.0
    sent = write_until(fd, b"\x00" * 4096, deadline)
    while out_queue(fd) > 0 and time.monotonic() < deadline:
        time.sleep(0.01)
    taken = sent - out_queue(fd)
    if taken == 4096:
        print("4. the chip took 4096 bytes in %d ms (something on it reads the port)"
              % ((time.monotonic() - start) * 1000))
        if not found and not rom:
            print("   it reads the port but did not answer - please also report whether"
                  " Zigbee2MQTT starts after this check")
    else:
        print("4. after 3 s the chip had taken only %d of 4096 bytes (nothing on it drains"
              " the USB receive buffer: firmware stalled or chip hung)" % max(taken, 0))
    termios.tcflush(fd, termios.TCOFLUSH)           # do not let close() wait for the rest
    os.close(fd)


def reset(path):
    fd = open_port(path)                            # kernel: DTR=1 RTS=1
    both = struct.pack("i", termios.TIOCM_DTR | termios.TIOCM_RTS)
    rts = struct.pack("i", termios.TIOCM_RTS)
    try:
        fcntl.ioctl(fd, termios.TIOCMBIC, both)     # RTS=0 DTR=0: clears the download-mode flag
        fcntl.ioctl(fd, termios.TIOCMBIS, rts)      # RTS=1 DTR=0: resets the chip
        time.sleep(0.1)
        fcntl.ioctl(fd, termios.TIOCMBIC, rts)      # RTS=0 DTR=0: chip boots from flash
    except OSError as e:
        os.close(fd)
        sys.exit("control-line request failed (%s): the chip's USB port did not accept it" % e.strerror)
    os.close(fd)
    print("reset sent to %s; the firmware is up again within a couple of seconds" % path)


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--reset":
        reset(args[1])
    elif len(args) == 1 and not args[0].startswith("-"):
        check(args[0])
    else:
        sys.exit("usage: usj_diag.py <port>   |   usj_diag.py --reset <port>")


if __name__ == "__main__":
    main()
