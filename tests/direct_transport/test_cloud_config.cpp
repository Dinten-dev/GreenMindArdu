#include "../../examples/direct_sensor/src/CloudConfig.h"
#include <cassert>
#include <string>
#include <iostream>

int main() {
    using namespace greenmind::cloud;
#ifdef GREENMIND_CLOUD_PRODUCTION
    assert(std::string(host) == "green-mind.ch");
    assert(std::string(preferencesNamespace) == "gmdirectprod");
    assert(!acceptsEndpoint("https://test.green-mind.ch/api/v1/direct-ingest/chunks"));
#else
    assert(std::string(host) == "test.green-mind.ch");
    assert(std::string(preferencesNamespace) == "gmdirect");
    assert(!acceptsEndpoint("https://green-mind.ch/api/v1/direct-ingest/chunks"));
#endif
    assert(acceptsEndpoint(std::string(base) + "/chunks"));
    assert(!acceptsEndpoint(std::string(endpoint) + "?redirect=1"));
    assert(!acceptsEndpoint(std::string("http://") + host + "/api/v1/direct-ingest/chunks"));
    assert(!acceptsEndpoint("https://green-mind.ch.attacker.invalid/api/v1/direct-ingest/chunks"));
    std::cout << "PASS: " << label << " target and separate credential namespace\n";
}
