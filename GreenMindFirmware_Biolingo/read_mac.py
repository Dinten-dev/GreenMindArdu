import serial
import sys
import re
import time

if len(sys.argv) < 2:
    print("Usage: read_mac.py <port>")
    sys.exit(1)

port = sys.argv[1]
try:
    ser = serial.Serial(port, 115200, timeout=0.5)
    # Toggle DTR/RTS to reset the board and get boot messages
    ser.dtr = False
    ser.rts = False
    time.sleep(0.1)
    ser.dtr = True
    ser.rts = True
    
    start_time = time.time()
    mac = None
    while time.time() - start_time < 10:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if line:
            # Check for MAC print
            match = re.search(r"MAC:\s*([0-9A-F:a-f]+)", line)
            if match:
                mac = match.group(1)
                break
    ser.close()
    if mac:
        print(mac.upper())
        sys.exit(0)
    else:
        sys.exit(1)
except Exception as e:
    sys.stderr.write(f"Error: {e}\n")
    sys.exit(2)
