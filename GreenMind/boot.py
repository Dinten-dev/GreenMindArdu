import network
import json
import uos
import machine

# Attempt to load config
try:
    with open("config.json", "r") as f:
        config = json.load(f)
except Exception:
    config = {}

is_provisioned = bool(config.get("wifi_ssid") and config.get("wifi_password"))
mac = network.WLAN(network.STA_IF).config("mac")
mac_str = "".join(["%02X" % b for b in mac])
mac_colon = ":".join(["%02X" % b for b in mac])
config["mac_address"] = mac_colon

# Make sure station and AP are handled
wlan_sta = network.WLAN(network.STA_IF)
wlan_ap = network.WLAN(network.AP_IF)

if is_provisioned:
    wlan_ap.active(False)
    wlan_sta.active(True)
    wlan_sta.connect(config["wifi_ssid"], config["wifi_password"])
else:
    # Setup Mode
    wlan_sta.active(False)
    wlan_ap.active(True)
    ap_ssid = f"GreenMind-Sensor-{mac_str[-4:]}"
    # Setting the mDNS hostname
    wlan_ap.config(essid=ap_ssid, authmode=0) # open AP
    print("Started Setup AP:", ap_ssid)
