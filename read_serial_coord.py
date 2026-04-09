import serial
import time
import sys

try:
    ser = serial.Serial('/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_58:E6:C5:E8:55:48-if00', 115200, timeout=1)
    
    # Soft Reset via DTR/RTS
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    
    print("Reset triggered on Coordinator, reading serial output for 15 seconds...")
    start = time.time()
    while time.time() - start < 15:
        line = ser.readline()
        if line:
            print(line.decode('utf-8', errors='ignore').strip())
            
    ser.close()
except Exception as e:
    print(f"Error: {e}")
