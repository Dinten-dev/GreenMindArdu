# Direct 2.12: begrenzte Einrichtung und WLAN-Wechsel

Lokaler Release-Kandidat auf Basis von `9dc2a49`. Noch nicht veröffentlicht
oder auf einen Sensor übertragen. Gilt für `examples/direct_sensor`,
einschliesslich dessen Direct-/DUAL-/seriell eingerichteter Gateway-Konfiguration.
Die separate Firmware `GreenMindFirmware_Biolingo` ist nicht Bestandteil dieser Änderung.

| Situation | Verhalten |
|---|---|
| WLAN beim Start erreichbar | Kein automatischer Einrichtungshotspot |
| Gespeichertes WLAN fehlt | 60 Sekunden Verbindungsversuche, danach Portal |
| Noch kein WLAN gespeichert | Portal unmittelbar verfügbar |
| Portal geöffnet | Höchstens 15 Minuten pro Fenster |
| Weiter offline | Neues Fenster eine Stunde nach dem vorherigen Fensterstart |
| WLAN kehrt zurück | Automatisches Fenster schliesst; Verbindung bleibt bestehen |
| BOOT fünf Sekunden halten und loslassen | Manuelles 15-Minuten-Fenster, auch bei bestehender Verbindung |
| Bereits eingerichteter Sensor | Nur WLAN-Name und Passwort; kein Pairing-Code |
| Ersteinrichtung | Weiterhin gültiger Direct-Code aus dem Dashboard erforderlich |

Der Sensor versucht das gespeicherte WLAN auch während geschlossener Fenster
weiter zu erreichen. Nach 15 Minuten folgen normalerweise 45 Minuten ohne
Hotspot. Das Erreichen des Portals oder wiederholtes Laden seiner Seite
verlängert das Fenster nicht. BOOT bleibt ein ausdrücklicher manueller Zugriff.

Beim WLAN-Wechsel werden neue Zugangsdaten zunächst nur im Arbeitsspeicher
ausprobiert. Erst ein neues IP-Ereignis für das angegebene WLAN bestätigt den
Wechsel; eine noch sichtbare alte Verbindung genügt nicht. Nach spätestens
90 Sekunden ohne Bestätigung wird wieder das bisherige WLAN versucht.
Ein ablaufendes Portalfenster bricht einen unbestätigten Wechsel ebenfalls ab.

Gespeichert werden ausschliesslich `ssid` und `password`. Geräte-ID, Token,
Pairingstatus, Betriebsart, Serveradresse, Zertifikat und Gateway bleiben
unverändert. Ein Code im WLAN-Wechselformular wird abgelehnt; dieser Weg führt
keine erneute Registrierung und keinen Organisationswechsel aus. Auch bereits
seriell konfigurierte Identitäten bleiben verwendbar.

Beim eingerichteten Sensor laufen Aufnahme, Upload-Aufgaben und Puffer weiter.
Der WLAN-Wechsel löst keinen Neustart und keine Pufferleerung aus. Nach einer
Ersteinrichtung beziehungsweise beim Ergänzen eines fehlenden WLANs ohne bereits
gestartete Laufzeit erfolgt weiterhin ein Neustart. Staging und Produktion
verwenden weiterhin getrennte Konfigurationsbereiche.

Die Portalrouten werden einmal installiert. Geschlossene Fenster stoppen HTTP,
DNS und ausschliesslich den Access Point; der WLAN-Client bleibt aktiv.
Zeitvergleiche sind auch beim Überlauf von `millis()` funktionsfähig. Während
einer bereits laufenden OTA-Wartung wird keine neue Einrichtung geöffnet.

Prüfung: native Zeitfenster- und Speichertests, 32 vorhandene Python-Tests sowie
Staging- und N16R8-Produktionsbuild. Der native Test umfasst einen simulierten
50-Tage-Ausfall, manuelle Fenster bei funktionierendem WLAN, Wiederverbindung,
leeres WLAN-Passwort und fehlgeschlagene Speicherung. Er prüft die unveränderten
Identitätsfelder mit einem simulierten Konfigurationsspeicher.

Ein physischer Sensor-/Routerwechsel wurde in diesem Auftrag nicht ausgeführt.
Die Builds bestätigen Kompilierbarkeit, nicht die vollständige Feldabnahme.

Native Prüfung:

```sh
g++ -std=c++17 -Wall -Wextra -Werror tests/direct_transport/test_setup_window.cpp -o /tmp/setup-window-test
/tmp/setup-window-test
```
