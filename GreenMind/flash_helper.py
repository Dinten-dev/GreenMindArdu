import sys, time, serial

PORT = "/dev/cu.usbserial-0001"
BAUD = 115200
FILE_TO_UPLOAD = "main.py"

print("==================================================")
print(" ESP32 FLASH HELPER v2: REPL Catcher")
print("==================================================")
print(f"Verbinde zu {PORT}...")

try:
    with serial.Serial(PORT, BAUD, timeout=0.1) as ser:
        print("\n\n>>> BITTE DRÜCKE JETZT DEN 'EN' ODER 'RST' KNOPF AUF DEM ESP32! <<<")
        print("(Warte auf den Neustart des Chips...)")
        
        booted = False
        start_wait = time.time()
        buf = ""
        while time.time() - start_wait < 30:
            data = ser.read(1024).decode('utf-8', errors='ignore')
            if data:
                buf += data
                
            # Wir warten auf den GENAUEN Moment, wo MicroPython läuft:
            if 'Booting' in buf or 'Connecting' in buf:
                print("\n[!] MicroPython läuft! Feure Ctrl-C...")
                
                # Sende 5x Ctrl-C mit kleinen Pausen
                for _ in range(10):
                    ser.write(b'\x03')
                    time.sleep(0.05)
                
                # Raw REPL mode aktivieren
                ser.write(b'\x01')
                time.sleep(0.1)
                booted = True
                break
            
        if not booted:
            print("\n[Fehler] Kein Neustart-Signal nach 30 Sekunden empfangen. Breche ab.")
            sys.exit(1)
            
        time.sleep(0.5)
        out = ser.read(4000)
        
        if b'raw REPL' in out or b'>>>' in out:
            print("[+] REPL erfolgreich abgefangen!")
            
            with open(FILE_TO_UPLOAD, "r") as f:
                code = f.read()
                
            print(f"[>] Lade {FILE_TO_UPLOAD} hoch...")
            cmd = b"f = open('" + FILE_TO_UPLOAD.encode() + b"', 'w')\n"
            ser.write(cmd)
            time.sleep(0.1)
            
            chunk_size = 256
            for i in range(0, len(code), chunk_size):
                chunk = code[i:i+chunk_size]
                chunk_escaped = chunk.replace('\\', '\\\\').replace("'", "\\'").replace('\n', '\\n').replace('\r', '')
                write_cmd = b"f.write('" + chunk_escaped.encode() + b"')\n"
                ser.write(write_cmd)
                time.sleep(0.05)
                
            ser.write(b"f.close()\n")
            time.sleep(0.1)
            
            ser.write(b'\x04')
            time.sleep(1)
            
            res = ser.read(4000)
            if b'Traceback' in res:
                print("[-] Fehler beim Schreiben:")
                print(res.decode('utf-8', errors='ignore'))
            else:
                print("\n[SUCCESS] Datei main.py erfolgreich mit 20Hz Sampling Rate geflasht!")
                print("ESP32 startet jetzt neu...")
                
            ser.write(b'\x02') 
            time.sleep(0.1)
            ser.write(b'\x04') 
        else:
            print("\n[-] Konnte die REPL immer noch nicht fangen.")
            print("Puffer war:", out[-200:])

except Exception as e:
    print(f"\n[Fehler] {e}")
    sys.exit(1)
