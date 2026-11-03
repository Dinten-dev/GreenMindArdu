// Run the actual network scheduler while ADC capture continues during every
// simulated HTTP request. Lost acknowledgements must not remove preview bins.
#include "Raw400Harness.h"
#include "../../examples/direct_sensor/src/CloudConfig.h"
#include <ArduinoJson.h>
#include <sstream>

struct String : std::string {
    using std::string::string;
    String() = default;
    String(const std::string& value) : std::string(value) {}
    String(unsigned long long n) : std::string(std::to_string(n)) {}
    String(unsigned short n) : std::string(std::to_string(n)) {}
    size_t write(uint8_t c) { push_back(static_cast<char>(c)); return 1; }
    size_t write(const uint8_t* p, size_t n) { append(reinterpret_cast<const char*>(p), n); return n; }
};
inline String operator+(const std::string& left, uint16_t value) { return String(left + std::to_string(value)); }
constexpr int WL_CONNECTED = 3, HTTPC_DISABLE_FOLLOW_REDIRECTS = 0;
struct { int status() { return WL_CONNECTED; } } WiFi;
struct {
    bool online = true;
    bool connected() { return online; }
    void stop() { online = false; }
} directTransport;
static greenmind::Mode mode = greenmind::Mode::Direct;
using greenmind::Mode;
static String deviceId = "00000000-0000-4000-8000-000000000001", token = "synthetic";
static const char sessionId[] = "00000000-0000-4000-8000-000000000002";
static const char generationId[] = "00000000-0000-4000-8000-000000000003";
static uint64_t nextSample = 1000000;
static uint32_t viewerUntil = 21000, controlRequests = 0, postRequests = 0, duplicates = 0;
static unsigned lostAcks = 0;
static std::string committedBody;
static uint32_t committedSequence = 0;
static std::vector<uint64_t> receivedFrames;

static void advance(uint32_t ms) {
    const uint64_t end = testTime + uint64_t(ms) * 1000;
    while (nextSample <= end) {
        testTime = nextSample;
        testAdc = (nextSample / 2500) % 4096;
        timerCallback(nullptr);
        Raw400Reading reading;
        assert(raw400Queue.pop(reading));
        appendRaw400(reading, 1800000000000000ULL, 900000);
        nextSample += 2500;
    }
    testTime = end;
}
struct {
    String path, response;
    bool begin(decltype(directTransport)&, const String& value) { path = value; return true; }
    void setReuse(bool) {}
    void setConnectTimeout(int value) { assert(value == 100); }
    void setTimeout(int value) { assert(value >= 500 && value <= 1000); }
    void setFollowRedirects(int) {}
    void addHeader(const char*, const String&) {}
    int getSize() { return response.size(); }
    String getString() { return response; }
    void end() {}
    String demandResponse(const char* status) {
        JsonDocument ack;
        if (status) ack["status"] = status;
        ack["enabled"] = millis() < viewerUntil;
        ack["expires_in_ms"] = millis() < viewerUntil ? viewerUntil - millis() : 0;
        ack["generation"] = generationId;
        String result;
        serializeJson(ack, result);
        return result;
    }
    int GET() {
        ++controlRequests;
        advance(550);
        response = demandResponse(nullptr);
        return 200;
    }
    int POST(const String& body) {
        ++postRequests;
        // Transport callbacks keep acquiring real samples during a slow request.
        advance(postRequests % 3 == 0 ? 650 : 450);
        if (millis() >= viewerUntil) {
            response = "{\"error\":\"live_not_requested\"}";
            return 409;
        }
        JsonDocument packet;
        assert(!deserializeJson(packet, body.c_str()));
        const uint32_t sequence = packet["sequence"];
        if (!committedBody.empty() && sequence == committedSequence) {
            assert(body == committedBody); // Same rail window and values, too.
            ++duplicates;
            response = demandResponse("duplicate");
            return 200;
        }
        for (JsonObject item : packet["points"].as<JsonArray>()) {
            const uint64_t frame = item["frame"];
            if (!receivedFrames.empty()) assert(frame == receivedFrames.back() + 32);
            assert(item["samples"] == 32);
            receivedFrames.push_back(frame);
        }
        committedSequence = sequence;
        committedBody = body;
        response = demandResponse("accepted");
        if ((lostAcks == 0 && receivedFrames.size() > 30) ||
            (lostAcks == 1 && receivedFrames.size() > 100)) {
            ++lostAcks;
            response = lostAcks == 1 ? "" : "{";
            return lostAcks == 1 ? -11 : 200; // Lost or truncated committed ACK.
        }
        return 200;
    }
} directHttp;
#include "../../examples/direct_sensor/src/DemandLiveNetwork.h"

int main() {
    try { acquireRaw400(); } catch (StopTask&) {}
    advance(10);
    size_t rawAcknowledged = 0;
    while (millis() < 26000) {
        if (submitted.size() > rawAcknowledged) {
            advance(150); // Raw uploads and retries get the same sole TLS worker.
            ++rawAcknowledged;
            directTransport.online = true;
        }
        directLiveTick(submitted.size() > rawAcknowledged);
        advance(10);
    }
    assert(duplicates == 2 && lostAcks == 2);
    assert(receivedFrames.size() >= 225);
    assert(liveDropped.load() == 0 && timingDropped.load() == 0);
    assert(raw400Stats.queueDropped == 0 && raw400Stats.missingSlots == 0);
    assert(!greenmind::liveDemand(millis(), liveUntil.load()));
    // Active push ACKs replace control polling; only idle checks remain later.
    assert(controlRequests >= 4 && controlRequests < 10);
    assert(submitted.size() >= 24 && rawAcknowledged >= 24);
    for (const auto& block : submitted) {
        for (uint16_t i = 0; i < block.frames; ++i) {
            const uint64_t frame = block.firstFrame + i;
            int16_t pcm; uint16_t mv;
            assert(greenmind::encodeRaw400((400 + frame) % 4096, pcm, mv));
            assert(block.payload[i * 2] == static_cast<uint8_t>(pcm));
            assert(block.payload[i * 2 + 1] == static_cast<uint8_t>(uint16_t(pcm) >> 8));
        }
    }
    printRaw400Stats();
    std::cout << "PASS: slow HTTP, exact lost-ACK retry, contiguous preview, raw priority and viewer stop\n";
}
