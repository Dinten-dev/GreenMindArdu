import os
import sys
import requests

TOKEN = os.environ.get("GREENMIND_TOKEN", "")
if not TOKEN:
    sys.exit("Error: Set GREENMIND_TOKEN environment variable")

BASE_URL = "https://green-mind.ch/api/v1"
FIRMWARE_BIN = ".pio/build/biolingo_v22/firmware.bin"

if not os.path.exists(FIRMWARE_BIN):
    sys.exit(f"Error: {FIRMWARE_BIN} not found. Please build the firmware first.")

url = f"{BASE_URL}/firmware/upload"
headers = {
    "Authorization": f"Bearer {TOKEN}",
    "Accept": "application/json"
}

data = {
    "version": "1.0.7",
    "board_type": "BIOLINGO_V22",
    "hardware_revision": "v22",
    "mandatory": "true",
    "changelog": "Fix heap fragmentation OOM crash causing empty JSON arrays to be sent"
}

print(f"Uploading firmware 1.0.7 to {url}...")

with open(FIRMWARE_BIN, "rb") as f:
    files = {"file": ("firmware.bin", f, "application/octet-stream")}
    
    response = requests.post(url, headers=headers, data=data, files=files)
    
    if response.status_code == 201:
        print("Success! Firmware uploaded.")
        print(response.json())
    else:
        print(f"Failed to upload. Status code: {response.status_code}")
        print(response.text)
