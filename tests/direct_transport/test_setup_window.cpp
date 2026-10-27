#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "../../transport_validation/SetupWindow.h"

using greenmind::SetupWindow;

static void timing(uint32_t boot) {
    SetupWindow window;
    assert(!window.update(boot, false, true));
    assert(!window.update(boot + 59999, false, true));
    assert(window.update(boot + 60000, false, true));
    assert(window.update(boot + 60000 + 899999, false, true));
    assert(!window.update(boot + 60000 + 900000, false, true));
    assert(!window.update(boot + 60000 + 3599999, false, true));
    assert(window.update(boot + 60000 + 3600000, false, true));
    // Recovered WLAN closes an automatic window, then a fresh outage gets grace.
    assert(!window.update(boot + 60000 + 3600001, true, true));
    assert(!window.update(boot + 60000 + 3600002, false, true));
    assert(window.update(boot + 120000 + 3600002, false, true));
}

struct FakePreferences {
    std::map<std::string, std::string> values;
    std::vector<std::string> writes;
    bool beginAllowed = true, failNextPassword = false, open = false;
    bool begin(const char* name, bool readonly) {
        assert(std::string(name) == "gmdirectprod" && !readonly);
        return open = beginAllowed;
    }
    std::string getString(const char* key) { assert(open); return values[key]; }
    void putString(const char* key, const std::string& value) {
        assert(open); writes.emplace_back(key);
        if (std::string(key) == "password" && failNextPassword) { failNextPassword = false; return; }
        values[key] = value;
    }
    void end() { assert(open); open = false; }
};

int main() {
    timing(0);
    timing(UINT32_MAX - 20000); // Rollover during the initial reconnect grace.
    timing(UINT32_MAX - 100000); // Rollover while the portal is open.
    SetupWindow longOutage;
    for (uint64_t elapsed = 0; elapsed < 50ULL * 86400000; elapsed += 60000) {
        const bool expected = elapsed >= 60000 && (elapsed - 60000) % 3600000 < 900000;
        assert(longOutage.update(static_cast<uint32_t>(elapsed), false, true) == expected);
    }
    SetupWindow fresh;
    assert(fresh.update(100, false, false)); // Unprovisioned: available immediately.
    assert(!fresh.update(900100, false, false));
    assert(!fresh.update(3600099, false, false));
    assert(fresh.update(3600100, false, false));
    SetupWindow online;
    for (uint32_t now = 0; now < 86400000; now += 10000) assert(!online.update(now, true, true));
    online.requestManual(86400000);
    assert(online.update(86400000, true, true)); // BOOT also works when WLAN is healthy.
    assert(!online.update(87300000, true, true));
    assert(!online.update(90000000, true, true));
    SetupWindow manual;
    manual.requestManual(50);
    assert(manual.update(50, false, true));
    manual.dismiss(); // Success or AP-start failure must not immediately reopen.
    assert(!manual.update(51, false, true));
    assert(!manual.update(3600049, false, true));
    assert(manual.update(3600050, false, true));

    FakePreferences store;
    store.values = {{"ssid", "Old network"}, {"password", "old-password"},
                    {"device_id", "existing-device"}, {"token", "private-existing-token"},
                    {"paired", "true"}, {"transport", "DUAL"}, {"gateway", "http://gateway"},
                    {"endpoint", "https://existing/chunks"}, {"ca", "existing-trust"},
                    {"pair_code", ""}};
    const auto original = store.values;
    assert(greenmind::saveWifiOnly(store, "gmdirectprod", std::string("New network"), std::string("")));
    assert(store.values.at("ssid") == "New network" && store.values.at("password").empty());
    for (const auto& [key, value] : original)
        if (key != "ssid" && key != "password") assert(store.values.at(key) == value);
    for (const auto& key : store.writes) assert(key == "ssid" || key == "password");
    const auto beforeFailure = store.values;
    store.failNextPassword = true;
    assert(!greenmind::saveWifiOnly(store, "gmdirectprod", std::string("Failed network"), std::string("new")));
    assert(store.values == beforeFailure);
    store.beginAllowed = false;
    assert(!greenmind::saveWifiOnly(store, "gmdirectprod", std::string("Another"), std::string("new")));
    assert(store.values == beforeFailure);
}
