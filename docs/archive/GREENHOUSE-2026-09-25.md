# Gewächshaus: geprüfter Firmware-Kandidat vom 24.09.2026

Gilt für **Biolingo v22, ESP32-S3-WROOM-1-N16R8, AD8232**. Kein ADS131M04-Treiber.
Die physische Abnahme im neuen Gewächshaus steht noch aus. Build/Test-Erfolg
ersetzt weder einen echten Neustart noch WLAN-Ausfalltests am Gerät.

| Weg | Projekt / Umgebung | Version | Einrichtung |
| --- | --- | --- | --- |
| Sensor → Raspberry Pi | `GreenMindFirmware_Biolingo` / `biolingo_v22` | 1.1.1 | BLE Security 1, danach Sensor-Zuordnung prüfen |
| Sensor → Production | `examples/direct_sensor` / `direct_biolingo_production` | direct-hotspot-v2.7-production | Hotspot, WLAN, Direct-Code von green-mind.ch |
| Sensor → Staging | `examples/direct_sensor` / `direct_biolingo_test` | direct-hotspot-v2.7-staging | Hotspot, WLAN, Direct-Code von test.green-mind.ch |

## Behobene Fehler

- USB-Flashplan des Gateway-Sensors: Anwendung jetzt korrekt bei `0x20000`,
  OTA-Metadaten bei `0xf000`. Arduino-Defaults waren `0x10000` / `0xe000` und
  widersprachen der vorhandenen Partitionstabelle. NVS und Spool werden nicht verschoben.
- Vollständige, typisierte Empfangsbestätigungen: Boot-/Sitzungskennung,
  Sequenznummer, Stichprobenzahl beziehungsweise Nutzdatenhash müssen passen.
  Ein fehlendes Sequenzfeld wird nicht mehr als gültige Null gewertet.
- Kein Upload-Stillstand nach etwa 24,9 Tagen durch ein ungesetztes Retry-Zeitlimit.
- Direct verwendet dieselbe gepinnte ArduinoJson-Version 7.4.3 wie der
  Gateway-Sensor. Der neue native Test reproduzierte mit 7.0.0 einen Absturz
  beim Entfernen/Wiederverwenden von JSON-Feldern; mit 7.4.3 besteht er.
- Die zuvor lokale OLED-Verbesserung ist enthalten: Bestätigungsintervall,
  letzter Empfang, Fehler und verworfene Samples statt bloßem HTTP-Erfolg.

## Installationsregeln

Nur den vollen Commit aus dem Feldpaket verwenden; nicht eine bewegliche Branchspitze.
PlatformIO Core 6.1.19 baut alle drei Varianten. WLAN und Geräte-Token gehören
ausschließlich in den Sensor, niemals in GitHub oder Buildflags.

Vor Änderungen an einem bereits benutzten Sensor: vollständiges 16-MB-Flashbackup
erstellen, Prüfsumme speichern und geschützt aufbewahren; es enthält Zugangsdaten.
Nicht blind `erase_flash` ausführen. Ein Modus-/Partitionswechsel ist ein bewusster
USB-Vorgang. `firmware.bin` allein ist kein universelles Erstinstallationsabbild.
Die Dateien und Adressen im `manifest.json` des Feldpakets müssen zusammenpassen.

Die Gateway-Variante 1.1.x verwendet Protokoll 3. **Zuerst den vorgesehenen neuen Pi
installieren und dessen `/api/v1/health` prüfen:** Versionen `[1,2,3]` und
`sequence_acknowledgement: true`. Alte Sensoren bleiben mit dem neuen Gateway
kompatibel; ein alter Pi ist nicht automatisch mit neuer Sensorfirmware kompatibel.
Keine dieser Dateien über bestehende automatische OTA-Verteilungen ausrollen.

Gateway-Sensor: BLE-Name `GM-XXXX` und Proof-of-Possession vom OLED benutzen.
Der BLE-Code ist **kein Dashboard-Code**. BLE stellt nur WLAN ein. Anschließend
mit dem Gateway-Werkzeug `tools/register_sensor.py` MAC-Adresse und frischen
Gateway-Sensor-Code aus der gewünschten Dashboard-Zone zuordnen. Das Tool
übernimmt keinen API-Schlüssel. Niemals den Direct-Code dafür verwenden.

Direct: `GreenMind-Sensor-XXXX` verbinden, `http://192.168.4.1` öffnen, 2,4-GHz-WLAN
und frischen Direct-Code aus der Zielzone eingeben. WLAN muss Internet, DNS und
Zeitsynchronisierung erlauben. Die Anzeige muss LIVE/CLOUD und bestätigte Pakete
zeigen. BOOT fünf Sekunden halten und loslassen öffnet erneute Einrichtung.
Staging und Production haben getrennte gespeicherte Identitäten.

## Grenzen und Abnahme vor Ort

- Gateway-Firmware: vorhandene Filter, dauerhafter LittleFS-Puffer, eigene
  USB-Stromversorgung. Geräte nicht über den Pi mit Strom versorgen.
- Direct: AD8232-Rohsignal ohne dieselben Gateway-Filter, kein OTA, ungefähr
  neun Sekunden RAM-Puffer. Längere Ausfälle und Stromverlust können Samples
  verlieren. Nicht als ausfallsicheres Langzeitarchiv freigeben.
- DUAL ist eine ausdrücklich konfigurierte Vergleichsoption; nicht der
  Standardaufbau. Beide Archive bleiben getrennt, keine automatische Deduplizierung.
- Zehn Minuten pro Weg beobachten: steigende bestätigte Pakete, keine steigenden
  Verlustzähler, richtige Zone, Messkurve und abrufbare 380-Hz-WAV-Datei.
- WLAN kurz unterbrechen, Wiederverbindung und Lückenanzeige prüfen. Neustart
  separat prüfen. Eine ungeprüfte SD-/Stromausfallgarantie wird nicht behauptet.
- Erst nach dieser Hardware-Abnahme den Kandidaten als Feldfreigabe behandeln.
