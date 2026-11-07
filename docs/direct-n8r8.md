# Direct-to-Cloud v2.15: N8R8

`direct_biolingo_n8r8_test` und `direct_biolingo_n8r8_production` sind für ESP32-S3-WROOM-1-N8R8 mit 8 MiB Flash und 8 MiB Octal-PSRAM. Das Board aktiviert `BOARD_HAS_PSRAM` und `qio_opi`. Der Direct-Spool reserviert bei ausreichendem freien PSRAM bis zu 4 MiB und lässt 1 MiB Reserve; die tatsächliche Allokation zeigt `direct_spool_used`, `capacity` und `psram` auf der seriellen Diagnose. PSRAM ist flüchtig und geht bei Stromausfall oder Neustart verloren.

Der N8R8-Build verwendet das bestehende 8-MiB-Ein-Slot-Layout und unterstützt **kein OTA**. Das N16R8-OTA-Layout darf nicht verwendet werden. Dies ist ein Softwarekandidat: Kompilierung und die bestehenden Softwaretests sind bestanden, ein N8R8-Hardwaretest wurde nicht durchgeführt. Die 400-Hz-Langzeit- und Produktionsempfänger-Abnahme müssen separat nachgewiesen werden.

Für bestehende Geräte das N8R8-Anwendungspaket verwenden: Vorher Hardware und PSRAM physisch identifizieren, den kompletten Flash dieses Geräts privat sichern und den aktiv verwendeten Anwendungsslot anhand seines bestehenden Layouts prüfen. `prepare_direct_upgrade.py` prüft das Paket, den Sensor und die Partitionstabelle und gibt erst dann manuell zu prüfende Befehle aus. Es schreibt selbst nicht auf den Sensor. Bei mehreren Slots muss der richtige aktive Offset unabhängig ermittelt werden. Zugangsdaten und private Backups gehören nicht auf GitHub.

Die passende Firmware und der Production-Empfänger müssen beide kompatibel sein. Ein Puffer kann abgewiesene Uploads nur vorübergehend auffangen, er behebt keinen HTTP-422-Fehler. Bereits verlorene Messwerte werden nicht wiederhergestellt.
