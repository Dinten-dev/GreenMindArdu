import machine
import time
import json
import uasyncio as asyncio
import socket
from ssd1306 import SSD1306_I2C
import urandom
import urequests

# Hardware Config
i2c = machine.I2C(0, scl=machine.Pin(22), sda=machine.Pin(21))
try:
    oled = SSD1306_I2C(128, 64, i2c)
except Exception:
    oled = None

adc = machine.ADC(machine.Pin(34))
adc.atten(machine.ADC.ATTN_11DB) # 0-3.3V

def display_text(lines):
    if not oled: return
    oled.fill(0)
    for i, line in enumerate(lines):
        oled.text(line, 0, i * 10)
    oled.show()

def save_config(conf):
    try:
        with open("config.json", "w") as f:
            json.dump(conf, f)
    except Exception as e:
        print("Save failed:", e)

# ── HTML CAPTIVE PORTAL ────────────────────────
HTML_SETUP = """<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sensor Setup</title>
<style>
body { font-family: -apple-system, sans-serif; background: #f0fdf4; margin: 0; padding: 20px; color: #1f2937; }
.card { background: white; padding: 25px; border-radius: 20px; max-width: 400px; margin: 40px auto; box-shadow: 0 4px 6px rgba(0,0,0,0.05); }
h2 { color: #10b981; margin-top: 0; }
label { font-size: 13px; font-weight: bold; color: #6b7280; display: block; margin-bottom: 5px; }
input { width: 100%; padding: 12px; margin-bottom: 15px; border: 1px solid #e5e7eb; border-radius: 10px; box-sizing: border-box; font-size: 16px; }
button { width: 100%; padding: 14px; background: #10b981; color: white; border: none; border-radius: 12px; font-size: 16px; font-weight: bold; cursor: pointer; }
</style></head><body>
<div class="card">
<h2>GreenMind Sensor</h2>
<p>Verbinde den Sensor mit dem WLAN.</p>
<form method="POST" action="/provision">
<label>WLAN Name (SSID)</label>
<input type="text" name="ssid" required placeholder="Mein Heimnetzwerk">
<label>WLAN Passwort</label>
<input type="password" name="password" required>
<label>Pairing Code (vom Dashboard)</label>
<input type="text" name="code" required placeholder="123456" pattern="[a-zA-Z0-9]+" maxlength="6" style="text-transform: uppercase;">
<button type="submit">Jetzt Speichern</button>
</form>
</div></body></html>"""

# ── CAPTIVE PORTAL DNS SERVER ──────────────────
# Redirects ALL DNS queries to 192.168.4.1 so smartphones
# auto-detect the captive portal and pop up the login page.
async def dns_server_loop():
    # Tiny DNS server: answers every query with 192.168.4.1
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind(('0.0.0.0', 53))
    udp.setblocking(False)
    print("DNS captive portal server started on :53")

    # 192.168.4.1 as 4 bytes
    portal_ip = bytes([192, 168, 4, 1])

    while True:
        try:
            data, addr = udp.recvfrom(512)
            if len(data) < 12:
                continue
            # Build minimal DNS response
            # Header: copy ID, set QR=1 + flags, QDCOUNT=1, ANCOUNT=1
            resp = bytearray(data[:2])  # Transaction ID
            resp += b'\x81\x80'         # Flags: QR=1, AA=1, RD=1, RA=1
            resp += data[4:6]           # QDCOUNT (copy)
            resp += b'\x00\x01'         # ANCOUNT = 1
            resp += b'\x00\x00'         # NSCOUNT = 0
            resp += b'\x00\x00'         # ARCOUNT = 0
            # Question section (copy verbatim)
            idx = 12
            while idx < len(data) and data[idx] != 0:
                idx += data[idx] + 1
            idx += 5  # null byte + QTYPE(2) + QCLASS(2)
            resp += data[12:idx]
            # Answer section
            resp += b'\xc0\x0c'         # Name pointer to question
            resp += b'\x00\x01'         # TYPE A
            resp += b'\x00\x01'         # CLASS IN
            resp += b'\x00\x00\x00\x3c' # TTL 60s
            resp += b'\x00\x04'         # RDLENGTH 4
            resp += portal_ip           # RDATA
            udp.sendto(bytes(resp), addr)
        except OSError:
            pass
        await asyncio.sleep_ms(10)

# ── SETUP MODE ─────────────────────────────────
async def setup_server_loop():
    display_text(["GreenMind Setup", "WLAN AP:", f"GM-Sensor-{mac_str[-4:]}", "", "192.168.4.1"])

    # Start DNS redirect in parallel
    asyncio.create_task(dns_server_loop())

    addr = socket.getaddrinfo('0.0.0.0', 80)[0][-1]
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(addr)
    s.listen(1)
    s.setblocking(False)

    print("Setup server listening on", addr)
    while True:
        try:
            cl, addr = s.accept()
            cl.settimeout(3.0)
            req = b""
            while True:
                chunk = cl.recv(1024)
                req += chunk
                if not chunk or b"\r\n\r\n" in req:
                    break
            
            # For POST: ensure we read the full body
            if b"POST" in req and b"\r\n\r\n" in req:
                header_end = req.index(b"\r\n\r\n") + 4
                # Parse Content-Length
                content_length = 0
                for line in req[:header_end].decode('utf-8').split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        content_length = int(line.split(":")[1].strip())
                        break
                # Read remaining body if needed
                body_so_far = len(req) - header_end
                while body_so_far < content_length:
                    try:
                        chunk = cl.recv(1024)
                        if not chunk:
                            break
                        req += chunk
                        body_so_far += len(chunk)
                    except:
                        break

            # Identify route
            req_str = req.decode('utf-8')

            # Captive portal detection endpoints → redirect to setup page
            if ("GET /hotspot-detect" in req_str or      # Apple
                "GET /generate_204" in req_str or         # Android
                "GET /gen_204" in req_str or              # Android alt
                "GET /connecttest.txt" in req_str or      # Windows
                "GET /ncsi.txt" in req_str or             # Windows NCSI
                "GET /redirect" in req_str or             # Firefox
                "GET /canonical.html" in req_str):        # Android alt
                cl.send("HTTP/1.0 302 Found\r\nLocation: http://192.168.4.1/\r\n\r\n")

            elif "GET / " in req_str or "GET /setup" in req_str:
                cl.send("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n" + HTML_SETUP)

            elif "POST /provision" in req_str:
                body = req_str.split("\r\n\r\n")[-1]
                # Proper URL-decode function for form data
                def urldecode(s):
                    s = s.replace('+', ' ')
                    result = []
                    i = 0
                    while i < len(s):
                        if s[i] == '%' and i + 2 < len(s):
                            try:
                                result.append(chr(int(s[i+1:i+3], 16)))
                                i += 3
                                continue
                            except ValueError:
                                pass
                        result.append(s[i])
                        i += 1
                    return ''.join(result)
                
                params = {}
                for kv in body.split("&"):
                    if "=" in kv:
                        k, v = kv.split("=", 1)  # split only on first =
                        params[urldecode(k)] = urldecode(v)

                config["wifi_ssid"] = params.get("ssid", "")
                config["wifi_password"] = params.get("password", "")
                config["pairing_code"] = params.get("code", "").upper().strip()
                save_config(config)

                cl.send("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\nErfolgreich gespeichert! Sensor startet neu...")
                time.sleep(1)
                machine.reset()

            elif "DELETE /provision" in req_str:
                config.pop("wifi_ssid", None)
                config.pop("wifi_password", None)
                config.pop("pairing_code", None)
                save_config(config)
                cl.send("HTTP/1.0 200 OK\r\n\r\n")
                time.sleep(1)
                machine.reset()

            else:
                # Any unknown URL → redirect to setup page
                cl.send("HTTP/1.0 302 Found\r\nLocation: http://192.168.4.1/\r\n\r\n")

            cl.close()
        except OSError:
            pass
        await asyncio.sleep(0.1)

# ── RUNTIME MODE ───────────────────────────────
async def runtime_loop():
    display_text(["Connecting...", config.get("wifi_ssid", "")[:12]])
    # Await WiFi
    retries = 30
    while not wlan_sta.isconnected() and retries > 0:
        await asyncio.sleep(1)
        retries -= 1
    
    if not wlan_sta.isconnected():
        display_text(["WiFi failed!", "Rebooting..."])
        await asyncio.sleep(5)
        machine.reset()

    gateway_ip = None
    cached_gw = config.get("gateway_ip")
    if cached_gw:
        display_text(["Trying cached GW", cached_gw])
        try:
            s = socket.socket()
            s.settimeout(2.0)
            s.connect((cached_gw, 80))
            s.send(b"GET /api/v1/health HTTP/1.0\r\nHost: gm\r\n\r\n")
            resp = s.recv(256)
            s.close()
            if b"hardware_id" in resp:
                gateway_ip = cached_gw
                display_text(["Gateway OK!", cached_gw])
        except:
            pass

    # UDP broadcast discovery
    if not gateway_ip:
        display_text(["Searching Gateway..", "UDP Broadcast"])
        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            udp_sock.setsockopt(socket.SOL_SOCKET, 0x20, 1)
        except:
            pass
        udp_sock.settimeout(2.0)
        for _ in range(5):
            try:
                udp_sock.sendto(b"DISCOVER_GREENMIND_GATEWAY", ("255.255.255.255", 50000))
                data, addr = udp_sock.recvfrom(1024)
                msg = data.decode('utf-8')
                if msg.startswith("GATEWAY_IP:"):
                    gateway_ip = msg.split(":")[1]
                    break
            except Exception:
                pass
            await asyncio.sleep(1)
        udp_sock.close()
    
    # Fallback: scan local subnet for gateway health endpoint
    if not gateway_ip:
        display_text(["UDP failed", "Scanning subnet.."])
        my_ip = wlan_sta.ifconfig()[0]
        subnet = my_ip.rsplit(".", 1)[0]  # e.g. "192.168.1"
        for host in range(1, 255):
            candidate = f"{subnet}.{host}"
            if candidate == my_ip:
                continue
            try:
                s = socket.socket()
                s.settimeout(0.3)
                s.connect((candidate, 80))
                s.send(b"GET /api/v1/health HTTP/1.0\r\nHost: gm\r\n\r\n")
                resp = s.recv(256)
                s.close()
                if b"hardware_id" in resp:
                    gateway_ip = candidate
                    display_text(["Found Gateway!", candidate])
                    break
            except:
                pass
    
    # Cache the gateway IP for next boot
    if gateway_ip and gateway_ip != cached_gw:
        config["gateway_ip"] = gateway_ip
        save_config(config)
    
    if not gateway_ip:
        display_text(["Gateway Missing!", "Rebooting.."])
        await asyncio.sleep(5)
        machine.reset()

    url = f"http://{gateway_ip}"
    mac = config["mac_address"]
    
    # Do registration if pairing code exists
    if config.get("pairing_code"):
        display_text(["Registering App..", url])
        try:
            r = urequests.post(f"{url}/api/v1/sensors/register", json={
                "mac_address": mac,
                "code": config["pairing_code"]
            }, timeout=3.0)
            status = r.status_code
            r.close()
            if status == 200 or status == 201:
                config.pop("pairing_code", None)
                save_config(config)
        except Exception as e:
            display_text(["Reg Failed!", "Rebooting..."])
            await asyncio.sleep(3)
            machine.reset()

    # Create delete listener port for Gateway to hit
    asyncio.create_task(runtime_delete_listener())

    
    display_text(["GreenMind OK", "IP: " + wlan_sta.ifconfig()[0], "Streaming..."])

    error_count = 0
    while True:
        # Read 20 samples at 20Hz (50ms) = 1.0s per batch
        # Matches Nyquist for ~10.6Hz RC low-pass filter (10µF + 1.5kΩ)
        batch = []
        for _ in range(20):
            val = adc.read()
            volts = val * 3.3 / 4095
            batch.append({
                "kind": "bio_signal",
                "value": round(volts, 4),
                "unit": "V"
            })
            await asyncio.sleep_ms(50)
        
        # Send to gateway using raw socket (urequests blocks asyncio)
        payload = json.dumps({
            "mac_address": mac,
            "readings": batch
        })
        try:
            s = socket.socket()
            s.settimeout(5.0)
            s.connect((gateway_ip, 80))
            req = (
                f"POST /api/v1/ingest HTTP/1.0\r\n"
                f"Host: {gateway_ip}\r\n"
                f"Content-Type: application/json\r\n"
                f"Content-Length: {len(payload)}\r\n"
                f"\r\n"
                f"{payload}"
            )
            s.send(req.encode())
            resp = s.recv(256)
            s.close()
            if b"200" in resp or b"201" in resp:
                error_count = 0
                display_text(["GreenMind Stream", "20Hz [ OK ]", f"{len(batch)} samples"])
            else:
                error_count += 1
                display_text(["Stream Warn", resp[:30].decode(), f"E:{error_count}"])
        except Exception as e:
            error_count += 1
            display_text(["Stream Error", str(e)[:16], f"Errors: {error_count}"])
            print("Post error:", e)
            if error_count > 20:
                display_text(["Too many errors", "Rebooting..."])
                await asyncio.sleep(3)
                machine.reset()
        
        # Minimal pause for task switching (no data gap)
        await asyncio.sleep_ms(10)

async def runtime_delete_listener():
    # A simple HTTP server to listen to DELETE commands in runtime mode
    addr = socket.getaddrinfo('0.0.0.0', 80)[0][-1]
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(addr)
    s.listen(1)
    s.setblocking(False)
    while True:
        try:
            cl, _ = s.accept()
            req = cl.recv(512).decode('utf-8')
            if "DELETE /" in req:
                cl.send("HTTP/1.0 200 OK\r\n\r\n")
                config.pop("wifi_ssid", None)
                config.pop("pairing_code", None)
                save_config(config)
                display_text(["Wiped!", "Rebooting..."])
                time.sleep(1)
                machine.reset()
            cl.close()
        except OSError:
            pass
        await asyncio.sleep(0.5)

try:
    if is_provisioned:
        asyncio.run(runtime_loop())
    else:
        asyncio.run(setup_server_loop())
except KeyboardInterrupt:
    print("Stopped")
except Exception as e:
    import sys
    # Log crash to file for debugging
    with open("crash.log", "w") as f:
        sys.print_exception(e, f)
    sys.print_exception(e)
    print("Crash logged to crash.log")
