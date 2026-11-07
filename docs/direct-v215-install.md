# Direct-to-Cloud v2.15: Installation als Kandidat

Die Live-Ansicht wurde mit einem separaten Sensor über den Hotspot auf Staging getestet und vom Nutzer bestätigt. Diese Production-Anwendungsabbilder wurden danach neu gebaut. Eine separate 400-Hz-Langzeitabnahme sowie der OTA-Vorwärts-, Rückwärts- und Rollback-Test dieser Bilder stehen aus. Die Pakete sind Vorabversionen, keine automatische Aktualisierung oder Freigabe für Peters Büroboxen.

## Hardware und Paket auswählen

- **8 MiB, ohne PSRAM:** `direct-v2.15-new-8mb.zip` für ein neues Gerät; `direct-v2.15-app-8mb.zip` für ein bestehendes Gerät. Kein OTA.
- **N16R8, 16 MiB Flash und 8 MiB PSRAM:** `direct-v2.15-n16r8-ota.zip`. OTA ist einkompiliert; es benötigt die passende Zwei-Slot-Partitionierung und einen kompatiblen Dienst. 16 MiB Flash allein belegt keinen N16R8/PSRAM.

Jedes Paket enthält Prüfsummen und Vorbereitungswerkzeuge. Keine Daten, Zugangsdaten oder Paarungen werden mitgeliefert. Weder das Prüfen noch die Veröffentlichung schreibt ein Gerät.

## Vorbereitung

1. ZIP und `SHA256SUMS` aus der gleichen GitHub-Veröffentlichung herunterladen und ZIP-Prüfsumme vergleichen.
2. Python und `esptool==4.11.0` installieren. USB-Port eindeutig bestimmen, serielle Monitore schliessen.
3. Flash-Grösse mit `python -m esptool --chip esp32s3 --port PORT flash_id` lesen. Für N16R8 PSRAM anhand der Hardware prüfen.
4. Vollständigen privaten Flash-Stand des konkreten Geräts mit `python -m esptool --chip esp32s3 --port PORT read_flash 0 SIZE PRIVATE_BACKUP` lesen: SIZE `0x800000` bei 8 MiB, `0x1000000` bei 16 MiB. Das Backup kann Passwörter enthalten und bleibt privat.
5. ZIP entpacken und im Paketordner den passenden Befehl ausführen. `PORT` und `PRIVATE_BACKUP` ersetzen. Das Werkzeug prüft Dateien, Speichergrösse und Partitionierung und **druckt** danach Schreib- und Prüfbefehle; es schreibt nichts selbst. Diese Befehle sorgfältig prüfen, dann ausführen.

Neue 8-MiB-Box (NVS muss leer sein):

```
python prepare_new_direct_sensor.py --backup PRIVATE_BACKUP --port PORT --new-unprovisioned
```

Bestehende 8-MiB-Box (nur Anwendung; bei mehreren Slots aktiven Slot separat ermitteln und `--app-offset 0x...` ergänzen):

```
python prepare_direct_upgrade.py --backup PRIVATE_BACKUP --port PORT
```

Neue N16R8-Box (gesamter Flash muss leer sein):

```
python prepare_new_direct_ota_sensor.py --backup PRIVATE_BACKUP --port PORT --new-unprovisioned
```

Bestehende N16R8-Box mit Zwei-Slot-OTA-Layout:

```
python prepare_direct_ota_upgrade.py --backup PRIVATE_BACKUP --port PORT
```

Bei unterstütztem altem Ein-Slot-Layout verweigert das Werkzeug die Migration ohne `--install-ota-layout`. Die Migration verändert die Partitionstabelle und benötigt eine gesonderte Prüfung; das private Backup bleibt erhalten. Unbekannte Layouts und ausstehende Teststarts werden abgelehnt. OTA-Updates erst nach Abnahme separat und gerätespezifisch zuweisen.

Nach USB-Installation Setup-Hotspot und Production-Paarung verwenden. Hardwarefehler, fehlende 400-Hz-Langzeitmessung und fehlender OTA-Rollback-Nachweis bleiben separate offene Punkte.
