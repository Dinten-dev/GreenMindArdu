#pragma once
#include <cstdint>

namespace greenmind {

// All elapsed-time comparisons remain valid across the millis() rollover.
class SetupWindow {
public:
    static constexpr uint32_t graceMs = 60UL * 1000;
    static constexpr uint32_t windowMs = 15UL * 60 * 1000;
    static constexpr uint32_t periodMs = 60UL * 60 * 1000;

    bool update(uint32_t now, bool networkReady, bool savedWifi) {
        if (open_) {
            if (now - openedAt_ >= windowMs || (networkReady && !manual_)) dismiss();
            if (networkReady) offline_ = cooling_ = false;
            return open_;
        }
        if (networkReady) {
            offline_ = cooling_ = false;
            return false;
        }
        if (!offline_) { offline_ = true; offlineAt_ = now; graceDone_ = !savedWifi; }
        if (cooling_ && now - openedAt_ < periodMs) return false;
        if (!graceDone_ && now - offlineAt_ < graceMs) return false;
        graceDone_ = true;
        open_ = true; manual_ = false; openedAt_ = now;
        return true;
    }

    void requestManual(uint32_t now) {
        open_ = manual_ = true;
        openedAt_ = now;
    }

    void dismiss() { open_ = manual_ = false; cooling_ = true; }

private:
    bool open_ = false, manual_ = false, offline_ = false, cooling_ = false, graceDone_ = false;
    uint32_t openedAt_ = 0, offlineAt_ = 0;
};

// No token, identity, pairing, transport, CA or gateway key is touched here.
template <class Preferences, class Text>
bool saveWifiOnly(Preferences& preferences, const char* space, const Text& ssid, const Text& password) {
    if (!preferences.begin(space, false)) return false;
    const auto oldSsid = preferences.getString("ssid");
    const auto oldPassword = preferences.getString("password");
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    const bool saved = preferences.getString("ssid") == ssid && preferences.getString("password") == password;
    if (!saved) {
        preferences.putString("ssid", oldSsid);
        preferences.putString("password", oldPassword);
    }
    preferences.end();
    return saved;
}
}
