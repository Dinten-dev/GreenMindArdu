#pragma once
#include <DNSServer.h>
#include <WebServer.h>
#include "StagingTrust.h"
#include "PairingCode.h"

static DNSServer portalDns;
static WebServer portalServer(80);
static bool portalActive = false, pairPending = false, portalCompleted = false;
static uint32_t pairStarted = 0, nextPairAttempt = 0;
static String portalNonce, portalMessage, portalName;
static String portalScreenMessage = "WLAN + Dashboard-Code";

static void portalPage() {
    String page = R"HTML(<!doctype html><html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>GreenMind einrichten</title><style>body{margin:0;background:#f3f6f0;color:#173b2c;font:17px system-ui}main{max-width:400px;margin:6vh auto;padding:28px;background:white;border-radius:22px}h1{font-size:29px}label{display:block;margin:20px 0 7px}input,button{box-sizing:border-box;width:100%;padding:14px;font:inherit;border:1px solid #bbcec0;border-radius:10px}button{margin-top:24px;background:#236b46;color:white;border:0}small{color:#546b5d}p{line-height:1.5}</style><main><small>GREENMIND · TESTUMGEBUNG</small><h1>Sensor verbinden</h1><p>Gib dein 2,4-GHz-WLAN und den Sensor-Code aus test.green-mind.ch ein.</p>)HTML";
    // Messages are fixed firmware text, never echoed form input or credentials.
    page += "<p role='status'>" + portalMessage + "</p>";
    if (pairPending) page += "<meta http-equiv='refresh' content='5'><p>Die Verbindung wird geprüft. Bitte dieses Fenster geöffnet lassen.</p>";
    else if (!portalCompleted) {
        page += "<form method='post' action='/save'><input type='hidden' name='nonce' value='" + portalNonce + "'>";
        page += R"HTML(<label for="ssid">WLAN-Name</label><input id="ssid" name="ssid" maxlength="32" autocomplete="off" required><label for="password">WLAN-Passwort</label><input id="password" name="password" type="password" maxlength="63" autocomplete="off"><label for="code">Sensor-Code aus dem Dashboard</label><input id="code" name="code" maxlength="8" minlength="6" pattern="([A-Za-z0-9]{6}|[A-Za-z0-9]{8})" title="Sensor-Code: 6 oder 8 Buchstaben/Ziffern" autocomplete="off" required><small>Dashboard → Sensor hinzufügen → Direkt über WLAN. Kein Konto-Passwort eingeben.</small><button>Verbinden</button></form>)HTML";
    }
    page += "<p><small>Bei Problemen: http://192.168.4.1 öffnen.</small></p></main></html>";
    portalServer.sendHeader("Cache-Control", "no-store");
    portalServer.send(200, "text/html; charset=utf-8", page);
}

static void savePendingPairing() {
    preferences.begin("gmdirect", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("device_id", deviceId);
    preferences.putString("token", token);
    preferences.putString("pair_code", pairingCode);
    preferences.putBool("paired", false);
    preferences.end();
}

static void startPortal() {
    if (portalActive) return;
    portalActive = true;
    WiFi.mode(WIFI_AP_STA);
    String suffix = WiFi.macAddress();
    suffix.replace(":", "");
    suffix = suffix.substring(suffix.length() - 4);
    portalName = "GreenMind-Sensor-" + suffix;
    if (!WiFi.softAP(portalName.c_str())) {
        portalActive = false;
        StatusDisplay::show("FEHLER", "Hotspot-Start fehlte", "Bitte neu starten", "", "");
        Serial.println("setup_hotspot_failed");
        return;
    }
    StatusDisplay::show("SETUP", portalName, "192.168.4.1", portalScreenMessage, "Mit Handy verbinden");
    portalNonce = String(esp_random(), HEX) + String(esp_random(), HEX);
    portalDns.start(53, "*", WiFi.softAPIP());
    portalServer.on("/", HTTP_GET, portalPage);
    portalServer.on("/save", HTTP_POST, []() {
        if (portalCompleted || pairPending || portalServer.arg("nonce") != portalNonce ||
            portalServer.arg("ssid").length() == 0 || portalServer.arg("ssid").length() > 32 ||
            portalServer.arg("password").length() > 63) {
            portalServer.send(400, "text/plain", "Bitte Formular erneut ausfuellen."); return;
        }
        String code = portalServer.arg("code"); code.trim(); code.toUpperCase();
        if (!greenmind::validPairingCode(code.c_str(), code.length())) {
            portalMessage = "Bitte den 6- oder 8-stelligen Sensor-Code eingeben: nur Buchstaben und Ziffern.";
            portalScreenMessage = "Sensor-Code pruefen";
            portalPage();
            return;
        }
        ssid = portalServer.arg("ssid"); password = portalServer.arg("password"); pairingCode = code;
        if (deviceId.length() != 36 || token.length() != 80) {
            makeSessionId(); deviceId = sessionId; makeSessionId();
            String compact = deviceId; compact.replace("-", "");
            token = "gmd_" + compact + "_";
            const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
            for (int i = 0; i < 43; ++i) token += alphabet[esp_random() & 63];
        }
        savePendingPairing();
        pairPending = true; pairStarted = millis(); nextPairAttempt = millis() + 2000;
        portalScreenMessage = "WLAN + Code pruefen";
        portalMessage = "WLAN wird verbunden und Sensor-Code geprüft.";
        portalPage();
        WiFi.begin(ssid.c_str(), password.c_str());
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    });
    portalServer.onNotFound(portalPage);
    portalServer.begin();
    Serial.printf("setup_hotspot=GreenMind-Sensor-%s address=192.168.4.1\n", suffix.c_str());
}

static void handlePortal() {
    if (!portalActive) return;
    portalDns.processNextRequest(); portalServer.handleClient();
    if (!pairPending) return;
    if (millis() - pairStarted > 90000) {
        pairPending = false;
        portalScreenMessage = "WLAN/Code pruefen";
        portalMessage = "Verbindung fehlgeschlagen. WLAN-Daten prüfen und erneut versuchen.";
        return;
    }
    if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000 ||
        int32_t(millis() - nextPairAttempt) < 0) return;
    nextPairAttempt = millis() + 10000;
    WiFiClientSecure tls; tls.setCACert(STAGING_CA); tls.setHandshakeTimeout(5);
    HTTPClient http; http.setConnectTimeout(3000); http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(tls, String(STAGING_BASE) + "/register")) return;
    http.addHeader("Content-Type", "application/json");
    JsonDocument data;
    data["code"] = pairingCode; data["device_id"] = deviceId;
    data["token"] = token; data["hardware_id"] = WiFi.macAddress();
    String body; serializeJson(data, body);
    const int status = http.POST(body);
    bool paired = false;
    if (status == 201 && http.getSize() >= 0 && http.getSize() < 1024) {
        JsonDocument response;
        paired = !deserializeJson(response, http.getString()) &&
            response["status"].as<String>() == "paired" && response["device_id"].as<String>() == deviceId;
    }
    http.end();
    Serial.printf("hotspot_pairing status=%d paired=%d\n", status, paired);
    if (paired) {
        preferences.begin("gmdirect", false);
        preferences.putString("transport", "DIRECT");
        preferences.putString("endpoint", String(STAGING_BASE) + "/chunks");
        preferences.putString("ca", STAGING_CA);
        preferences.putBool("paired", true);
        preferences.remove("pair_code"); preferences.end();
        portalMessage = "Sensor verbunden. Er startet jetzt und sendet an die Testumgebung.";
        pairPending = false; portalCompleted = true;
        StatusDisplay::show("BEREIT", "Sensor verbunden", "Neustart...", "test.green-mind.ch", "");
        // Leave enough time for the browser's status refresh before shutting AP.
        for (int i = 0; i < 700; ++i) { portalDns.processNextRequest(); portalServer.handleClient(); delay(10); }
        ESP.restart();
    } else if (status == 400 || status == 409 || status == 422) {
        pairPending = false;
        portalScreenMessage = "Code im Handy pruefen";
        portalMessage = status == 409 ? "Sensor oder Code bereits verbunden. Bitte den bestehenden Sensor im Dashboard prüfen." : "Sensor-Code ungültig oder abgelaufen. Bitte einen neuen Direct-Code im Dashboard erstellen.";
    } else if (status == 503 || status == 404) {
        pairPending = false;
        portalScreenMessage = "Server nicht bereit";
        portalMessage = "Die Testumgebung ist noch nicht für die Einrichtung bereit. Bitte später erneut versuchen.";
    }
}
