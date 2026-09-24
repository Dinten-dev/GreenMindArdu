# Direct-to-Cloud auf Biolingo v22 flashen und verbinden

Für **Biolingo v22 / AD8232 / ESP32-S3 mit 16 MB Flash**.
Ziel: **Production unter https://green-mind.ch**.
Firmware: `direct-hotspot-v2.7-production`, Quellcommit
`ae645ebc8a6818f37d1e6c39e37ea47c669f5760`.

## 1. Firmware direkt von GitHub herunterladen

[Geprüftes Firmware-Paket auf GitHub](https://github.com/Dinten-dev/GreenMindArdu/releases/tag/field-2026-09-25)
· [Production-ZIP herunterladen](https://github.com/Dinten-dev/GreenMindArdu/releases/download/field-2026-09-25/direct-production-2.7.zip)
· [Quellcode auf main](https://github.com/Dinten-dev/GreenMindArdu/tree/main)

Das Paket enthält vier Binärdateien, `manifest.json` und eine Feldanleitung.
Für jeden Sensor ist später ein eigener Dashboard-Code nötig.
Die Firmware enthält keine WLAN-Passwörter.

Auf dem Mac wird **Python 3.12** benötigt. Falls noch nicht installiert:
über [python.org](https://www.python.org/downloads/macos/) Python 3.12 installieren
oder mit vorhandenem Homebrew `brew install python@3.12` ausführen.
Anschließend Terminal öffnen. Alle folgenden Schritte im selben Terminal ausführen.
Bei einer Fehlermeldung stoppen, nicht mit dem Flashen fortfahren.

Zunächst eine eigene Arbeitsumgebung erstellen:

```bash
python3.12 --version
GM_FLASH_DIR=$(mktemp -d "$HOME/GreenMind-Direct-2.7.XXXXXX")
cd "$GM_FLASH_DIR"
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install esptool==4.11.0
```

Firmware und Prüfsummen direkt von GitHub laden und vor dem Entpacken prüfen:

```bash
python - <<'PYDOWNLOAD'
import hashlib
from pathlib import Path
from urllib.request import urlopen
from zipfile import ZipFile
base = 'https://github.com/Dinten-dev/GreenMindArdu/releases/download/field-2026-09-25/'
name = 'direct-production-2.7.zip'
for filename in ('SHA256SUMS', name):
    with urlopen(base + filename, timeout=120) as response:
        Path(filename).write_bytes(response.read())
checks = {line.split()[1].lstrip('*'): line.split()[0]
          for line in Path('SHA256SUMS').read_text().splitlines() if line.strip()}
actual = hashlib.sha256(Path(name).read_bytes()).hexdigest()
if checks.get(name) != actual:
    raise SystemExit('STOPP: ZIP-Pruefsumme stimmt nicht.')
with ZipFile(name) as package:
    expected = {'README.md', 'manifest.json', 'bootloader.bin',
                'partitions.bin', 'boot_app0.bin', 'firmware.bin'}
    if set(package.namelist()) != expected or len(package.namelist()) != len(expected):
        raise SystemExit('STOPP: Unerwarteter Paketinhalt.')
    package.extractall('firmware')
print('GitHub-Download und ZIP-Pruefsumme erfolgreich geprueft.')
PYDOWNLOAD
```

Nur nach der Erfolgsmeldung in den Firmware-Ordner wechseln:

```bash
cd "$GM_FLASH_DIR/firmware"
```

## 2. Sensor erkennen und vorhandene Daten sichern

Sensor mit einem **USB-Datenkabel** am Mac anschließen. Andere ESP32-Geräte
abziehen und offene serielle Monitore schließen. Ports anzeigen:

```bash
python -m serial.tools.list_ports
```

Den tatsächlichen Sensor-Port eingeben; keinen Port aus einem Beispiel übernehmen:

```bash
printf 'Sensor-Port aus der Liste eingeben: '
read -r SENSOR_PORT
python -m esptool --chip esp32s3 --port "$SENSOR_PORT" flash_id
```

Die Ausgabe muss ESP32-S3 und **16 MB** Flash bestätigen. Bei abweichender Hardware
hier stoppen. Wenn keine Verbindung möglich ist: BOOT gedrückt halten, RESET
kurz drücken, dann BOOT loslassen und den Port erneut prüfen.

Vollständiges Backup anlegen. Es enthält gegebenenfalls WLAN- und Gerätedaten:

```bash
umask 077
BACKUP_DIR=$(mktemp -d "$HOME/GreenMind-Sensor-Backup.XXXXXX")
python -m esptool --chip esp32s3 --port "$SENSOR_PORT" --after no_reset read_flash 0x0 0x1000000 "$BACKUP_DIR/flash-before.bin"
shasum -a 256 "$BACKUP_DIR/flash-before.bin" > "$BACKUP_DIR/SHA256SUMS"
```

Dieses Backup privat behalten, nicht auf GitHub laden. Kein `erase_flash` nötig.
Der folgende Schritt wechselt bewusst die Firmware und deren Partitionstabelle.
Ein Wechsel von Gateway zu Direct ist kein gewöhnliches OTA-Update.

## 3. Paket prüfen und flashen

Prüfsummen aller vier Dateien mit dem Manifest vergleichen:

```bash
python - <<'PY'
import hashlib, json
from pathlib import Path
m = json.loads(Path('manifest.json').read_text())
assert m['environment'] == 'direct_biolingo_production'
assert m['source_commit'] == 'ae645ebc8a6818f37d1e6c39e37ea47c669f5760'
assert {f['file']: f['offset'] for f in m['files']} == {
    'bootloader.bin': '0x0', 'partitions.bin': '0x8000',
    'boot_app0.bin': '0xe000', 'firmware.bin': '0x10000',
}
for f in m['files']:
    data = Path(f['file']).read_bytes()
    assert len(data) == f['bytes']
    assert hashlib.sha256(data).hexdigest() == f['sha256'], f['file']
print('Production-Paket und Flash-Adressen geprüft.')
PY
```

Nur nach erfolgreicher Prüfung:

```bash
python -m esptool --chip esp32s3 --port "$SENSOR_PORT" --baud 460800 --after no_reset write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
python -m esptool --chip esp32s3 --port "$SENSOR_PORT" --after hard_reset verify_flash 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

Esptool muss die Dateien erfolgreich verifizieren. Danach startet der Sensor.
Die obigen Adressen gelten **nur für dieses Direct-Paket**. Das Gateway-Paket
verwendet andere Adressen; Dateien und Befehle nicht mischen.

## 4. Direct-Code im Dashboard erzeugen

1. https://green-mind.ch/de/app/sensors öffnen und anmelden.
2. Falls nötig zuerst die neue Gewächshaus-Zone anlegen lassen.
3. **Sensor verbinden** auswählen.
4. Verbindungsart **Direkt über WLAN (Direct-Firmware)** auswählen.
5. Die richtige Gewächshaus-Zone auswählen.
6. **Code Erstellen** drücken.
7. Den angezeigten sechsstelligen Code notieren und sofort verwenden.

Der Code wird **von der Dashboard-Software erzeugt**. Er wird nicht in den
Firmware-Quellcode geschrieben. Er verknüpft diesen Sensor mit Firma und Zone.
Nicht das Konto-Passwort, BLE-Passwort oder einen Gateway-Code verwenden.
Bei Ablauf einen neuen Direct-Code erzeugen; nicht denselben Code für mehrere
Sensoren verwenden. Die Anzeige im Dashboard nennt die Gültigkeit.

## 5. Sensor mit WLAN und Cloud verbinden

1. Am Handy mit **GreenMind-Sensor-XXXX** verbinden.
2. Bei „Kein Internet“ im Sensornetz bleiben.
3. Im Browser **http://192.168.4.1** öffnen.
4. Prüfen, dass **PRODUCTION / green-mind.ch** angezeigt wird.
5. Den Namen des **2,4-GHz-WLANs** und dessen Passwort eingeben.
6. Den frisch erzeugten **Direct-Sensor-Code** eingeben.
7. **Verbinden** drücken und die Bestätigung abwarten.
8. Der Sensor startet neu; das Handy wieder normal verbinden.

Ein bereits eingerichteter Production-Sensor kann nach dem Flashen seine alte
Zuordnung behalten und direkt senden. Für eine neue Einrichtung **BOOT fünf
Sekunden halten und loslassen**. Bestehende Geräteidentität nicht eigenmächtig
löschen: Ein Wechsel in eine andere Firma/Zone muss passend im Dashboard erfolgen.

## 6. Prüfen, ob wirklich Daten ankommen

- OLED: LIVE/CLOUD, steigende bestätigte Pakete, ungefähr eine Bestätigung pro Sekunde.
- Keine dauerhaft steigenden Verlustzähler; `KEIN OK` ist keine erfolgreiche Verbindung.
- Im Dashboard richtige Zone öffnen und die Direct-Sensorkarte aufklappen.
- Unter Messungen aktuelle Kurve und Messzeit prüfen.
- Nach ausreichend Daten eine WAV-Datei öffnen/herunterladen und 380 Hz prüfen.
- Mindestens zehn Minuten beobachten, anschließend einen kontrollierten Neustart prüfen.

Internet, DNS und Zeitsynchronisierung müssen im WLAN verfügbar sein. Ohne
gültige Zeit beginnt Direct nicht zu messen. Ein sichtbarer Sensor-Eintrag allein
beweist noch keinen vollständigen Datenfluss.

## Grenzen für morgen

Die Softwaretests sind bestanden; dieser neue Build wurde hier noch nicht auf
einem angeschlossenen Sensor physisch abgenommen. Direct puffert ungefähr neun
Sekunden im RAM. Bei längeren Ausfällen oder Stromverlust sind Datenlücken möglich.
Für lange Offline-Zeiten ist der Gateway-Weg mit dauerhaftem Puffer vorzuziehen.
