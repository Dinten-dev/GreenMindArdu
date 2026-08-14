import re

with open("src/main.cpp", "r") as f:
    code = f.read()

# 1. Includes
code = code.replace("#include <WebServer.h>\n", "")
code = code.replace("#include <DNSServer.h>\n", "")
code = code.replace("#include <WiFi.h>\n", "#include <WiFi.h>\n#include <WiFiProv.h>\n")

# 2. Globals
code = code.replace("WebServer   server(80);\n", "")
code = code.replace("DNSServer   dnsServer;\n", "")
code = code.replace("static unsigned long lastWifiCheck    = 0;\n", "static unsigned long lastWifiCheck    = 0;\nstatic unsigned long setupModeStartTime = 0;\n")

# 3. HTML Portal -> generatePairingCode
html_start = code.find("// ── HTML Captive Portal")
html_end = code.find("// ══════════════════════════════════════════════\n//  SETUP & LOOP")
if html_start != -1 and html_end != -1:
    new_helpers = """// ── Helpers für BLE Provisioning ──────────────
String generatePairingCode() {
    String code = "";
    for (int i = 0; i < 6; i++) {
        int r = random(0, 36);
        if (r < 10) code += String(r);
        else code += String((char)('A' + r - 10));
    }
    return code;
}

"""
    code = code[:html_start] + new_helpers + code[html_end:]

# 4. loop()
loop_orig = """void loop() {
    if (!isProvisioned) {
        dnsServer.processNextRequest();
        server.handleClient();
    } else {"""
loop_new = """void loop() {
    if (!isProvisioned) {
        if (WiFi.status() == WL_CONNECTED && wifiSSID.length() == 0) {
            wifiSSID = WiFi.SSID();
            wifiPass = WiFi.psk();
            Serial.printf("[Biolingo] Provisioned successfully! SSID: %s\\n", wifiSSID.c_str());
            saveConfig();
            delay(1000);
            ESP.restart();
        }
        if (millis() - setupModeStartTime > 300000) {
            Serial.println("[Biolingo] 5 min timeout. Rebooting...");
            ESP.restart();
        }
        delay(100);
    } else {"""
code = code.replace(loop_orig, loop_new)

# 5. startSetupMode()
setup_orig_start = code.find("//  SETUP MODE (Captive Portal)")
setup_orig_end = code.find("// ══════════════════════════════════════════════\n//  RUNTIME MODE")
if setup_orig_start != -1 and setup_orig_end != -1:
    new_setup = """//  SETUP MODE (BLE Provisioning)
// ══════════════════════════════════════════════

void startSetupMode() {
    Serial.println("[Biolingo] Starting Setup Mode (BLE Provisioning)");

    String suffix = macAddress.substring(macAddress.length() - 5);
    suffix.replace(":", "");
    String bleName = "GM-" + suffix;

    String generatedCode = generatePairingCode();
    Serial.printf("[Biolingo] BLE Name: %s  Code: %s\\n", bleName.c_str(), generatedCode.c_str());

    Display::showBleProvisioning(bleName, generatedCode);
    
    setupModeStartTime = millis();
    WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM, WIFI_PROV_SECURITY_1, generatedCode.c_str(), bleName.c_str());
}

"""
    code = code[:setup_orig_start] + new_setup + code[setup_orig_end:]

# 6. startRuntimeMode() - Health + reset endpoints
runtime_http = """    // Health + reset endpoints
    server.on("/", HTTP_DELETE, []() {
        server.send(200, "text/plain", "OK");
        delay(500);
        factoryReset();
    });
    server.on("/health", HTTP_GET, []() {
        String json = "{\\"status\\":\\"ok\\",\\"version\\":\\"" + String(FIRMWARE_VERSION)
                     + "\\",\\"board\\":\\"BIOLINGO_V22\\",\\"ota\\":true}";
        server.send(200, "application/json", json);
    });
    server.begin();"""
code = code.replace(runtime_http, "    // HTTP endpoints removed for Phase 2")

# 7. streamReadings()
code = code.replace("    server.handleClient();\n", "")

with open("src/main.cpp", "w") as f:
    f.write(code)

print("Patched main.cpp")
