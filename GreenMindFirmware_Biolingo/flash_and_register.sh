#!/usr/bin/env bash
# ============================================================================
# Flash sensor with v1.0.4 + auto-provision, then register in production DB
# Usage: ./flash_and_register.sh [sensor_name]
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIO="/opt/anaconda3/bin/pio"
DEPLOY_KEY="$(cd "$SCRIPT_DIR/../../GreenMindDB/dev-tools" && pwd)/greenmind_deploy_key"
SERVER="traver@188.245.247.156"
ZONE_ID="c18595ab-f758-4199-aee5-cf2302a5e6f4"  # Gery Gewächshaus
GATEWAY_ID="90a10ee0-f0f3-4775-b087-4c5fcfc5c872"  # Reihe-28

# Find USB port
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
    echo "❌ Kein Sensor per USB gefunden. Anschliessen und nochmal versuchen."
    exit 1
fi
echo "📡 Sensor gefunden auf: $PORT"

# 1. Flash firmware
echo "🔧 Flashe v1.0.4 mit Auto-Provisioning..."
cd "$SCRIPT_DIR"
$PIO run --target upload --upload-port "$PORT" 2>&1 | tail -5

if [ $? -ne 0 ]; then
    echo "❌ Flash fehlgeschlagen!"
    exit 1
fi
echo "✅ Firmware geflasht"

# 2. Read MAC from serial output (wait for boot message)
echo "📖 Lese MAC-Adresse..."
sleep 2  # Wait for reboot

MAC=$($PIO device monitor --port "$PORT" --baud 115200 --filter direct 2>/dev/null | timeout 8 grep -m1 "\[Biolingo\] MAC:" | sed 's/.*MAC: \([0-9A-F:]*\).*/\1/')

if [ -z "$MAC" ]; then
    echo "⚠️  MAC konnte nicht automatisch gelesen werden."
    echo "    Bitte MAC-Adresse manuell eingeben (z.B. 14:C1:9F:43:79:E4):"
    read -r MAC
fi

echo "📡 MAC: $MAC"

# 3. Generate sensor name
SUFFIX="${MAC: -5}"
SUFFIX="${SUFFIX//:/-}"
SENSOR_NAME="${1:-Sensor-$SUFFIX}"
echo "📝 Name: $SENSOR_NAME"

# 4. Register in production DB
echo "☁️  Registriere in Produktion..."
ssh -i "$DEPLOY_KEY" -o StrictHostKeyChecking=no "$SERVER" "
cd /home/traver/greenmind-prod
COMPOSE_PROJECT_NAME=greenminddb docker compose -f docker-compose.prod.yml exec -T postgres psql -U admin -d plantdb -c \"
INSERT INTO sensor (id, name, mac_address, gateway_id, status, created_at, updated_at)
VALUES (
    gen_random_uuid(),
    '${SENSOR_NAME}',
    '${MAC}',
    '${GATEWAY_ID}',
    'online',
    NOW(),
    NOW()
)
ON CONFLICT (mac_address) DO UPDATE SET
    gateway_id = '${GATEWAY_ID}',
    status = 'online',
    updated_at = NOW();
\"
"

echo ""
echo "✅ Fertig! Sensor '$SENSOR_NAME' ($MAC)"
echo "   WiFi: GreenMind (auto-provisioned)"
echo "   Gateway: Gery Gewächshaus (Reihe-28)"
echo "   Dashboard: https://green-mind.ch/app/sensors"
