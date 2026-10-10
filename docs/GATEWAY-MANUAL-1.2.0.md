# Gateway-Sensor v1.2.0: manuelle Installation

Für Biolingo v22 / ESP32-S3-WROOM-1-N16R8. Der Sensor benötigt einen Raspberry-Pi-Gateway. Direct v2.8 bleibt eine getrennte Firmware.

[Manueller GitHub-Release](https://github.com/Dinten-dev/GreenMindArdu/releases/tag/manual-2026-10-10)
· [Gateway-Sensor-Paket](https://github.com/Dinten-dev/GreenMindArdu/releases/download/manual-2026-10-10/gateway-sensor-1.2.0.zip)

Die Binärdateien wurden kompiliert und automatisch geprüft. OLED, Hotspot, Verifizierung und Funkbetrieb müssen vor einem Feldrollout am realen Sensor geprüft werden. Es erfolgt keine OTA-Verteilung und kein Update vorhandener Sensoren.

## Installation

Paket und SHA256SUMS herunterladen, ZIP-Prüfsumme und alle Binär-Prüfsummen im manifest.json prüfen. Vorhandene NVS und ausstehende Sensordaten vor einem Variantenwechsel sichern. Keine Flash-Löschung durchführen. Für die unveränderte Gateway-Partitionstabelle gelten:

| Datei | Adresse |
|---|---|
| bootloader.bin | 0x0 |
| partitions.bin | 0x8000 |
| boot_app0.bin | 0xf000 |
| firmware.bin | 0x20000 |

Alternativ den Quellstand dieses Releases herunterladen und den bestehenden `./flash-sensor.sh` verwenden. Direct-Pakete haben andere Adressen; nicht mischen.

## Einrichten

1. Gateway mit dem Dashboard-Verifizierungscode registrieren.
2. Im Dashboard für dieses Gateway einen Sensor-Code erzeugen.
3. Mit `GreenMind-Sensor-XXXX` verbinden und `http://192.168.4.1` öffnen.
4. WLAN, Passwort und **Gateway-Sensor-Code** eingeben.
5. Registrierung und erfolgreiche Datenbestätigungen am Gateway prüfen.

BOOT fünf Sekunden halten und loslassen öffnet die Einrichtung erneut. WLAN-Daten, Verifizierungscode und Gateway-Adresse werden unter den bisherigen NVS-Namen gespeichert. Der ausstehende Messdatenpuffer bleibt erhalten.

## Änderungen und Notwendigkeit

| Änderung | Bedeutung für Peters Probleme |
|---|---|
| OLED: Sensor-ID und Mittelwert/Min/Max der letzten Sekunde | Diagnose; behebt keine Übertragungsverluste |
| Lokaler Hotspot mit Verifizierungscode | Nötig für korrekte Einrichtung neuer Gateway-Sensoren |
| OTA-Prüfungen deaktiviert | Erzwingt manuelle Installation dieser Downloadvariante |
| Sampling, Filter, Gateway-Transport, Puffer und ACK | Unverändert |

WAV-Warnungen und die korrekte `wss://`-Adresse gehören zur separaten Raspberry-Pi-Software. Deren Installation erfolgt im vereinbarten späteren Wartungsfenster. Die Serveroptimierung erfordert kein Sensor-Update. Die genaue Ursache der alten Büro-Funkverluste ist ohne laufenden Büro-Gateway nicht nachgewiesen.
