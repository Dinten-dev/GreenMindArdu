#ifndef GREENMIND_OTA_CLIENT_H
#define GREENMIND_OTA_CLIENT_H

#include <Arduino.h>

class GreenMindOTA {
public:
    /// Check gateway for available firmware update and apply if found.
    static void checkAndUpdate(const String& gatewayIp);

    /// Report update status back to gateway (which forwards to cloud).
    static void reportStatus(const String& gatewayIp, const String& releaseId,
                             const char* status, const char* errorMessage = nullptr);

private:
    static bool performDownload(const String& fullUrl, const String& expectedSha256);
};

#endif
