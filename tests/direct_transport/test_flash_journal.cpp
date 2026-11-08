#include "../../direct_transport/FlashJournal.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace greenmind;
namespace fs = std::filesystem;
static SampleBlock sample(uint64_t sequence) {
    SampleBlock b; b.sequence = sequence; b.firstFrame = sequence * 400;
    b.frames = b.sampleRate = 400;
    for (size_t i = 0; i < b.byteCount(); ++i) b.payload[i] = uint8_t(sequence + i);
    return b;
}
static fs::path first(const fs::path& dir) {
    std::vector<fs::path> names;
    for (auto& p : fs::directory_iterator(dir)) if (p.path().extension() == ".gmfj") names.push_back(p.path());
    assert(!names.empty()); std::sort(names.begin(), names.end()); return names.front();
}
int main() {
    char pattern[] = "/tmp/greenmind-flash-journal-XXXXXX";
    char* temp = mkdtemp(pattern); assert(temp);
    const std::string root(temp), dir = root + "/normal", meta = "{\"session\":\"original\",\"firmware\":\"v1\"}";
    {
        FlashJournal j; assert(j.begin(dir.c_str(), 1024 * 1024));
        for (unsigned i = 0; i < 120; ++i) assert(j.append(sample(i), meta));
        SampleBlock read; std::string identity;
        assert(j.peek(read, identity) && identity == meta && read.sequence == 0);
        assert(!j.acknowledge(1)); assert(j.acknowledge(0));
        assert(j.peek(read, identity) && read.sequence == 1); // No ACK: reboot must replay it.
    }
    {
        FlashJournal j; assert(j.begin(dir.c_str(), 1024 * 1024));
        // Metadata remains unchanged across firmware/session changes and a restart.
        assert(j.append(sample(0), "{\"session\":\"new\",\"firmware\":\"v2\"}"));
        SampleBlock read; std::string identity;
        for (unsigned i = 0; i < 120; ++i) {
            assert(j.peek(read, identity) && identity == meta && read.sequence == i);
            assert(read.firstFrame == i * 400 && read.byteCount() == 800);
            assert(!std::memcmp(read.payload, sample(i).payload, 800));
            assert(j.acknowledge(i));
        }
        assert(j.peek(read, identity) && identity.find("new") != std::string::npos && read.sequence == 0);
        assert(j.acknowledge(0)); assert(!j.peek(read, identity)); assert(!j.failed());
        assert(j.pendingBytes() == 0 && j.used() > 0); // ACKed active file must not suppress Live.
    }
    {
        const auto path = root + "/bounded";
        FlashJournal j; assert(j.begin(path.c_str(), 140000));
        unsigned count = 0; while (j.append(sample(count), meta)) ++count;
        assert(count && !j.failed());
        SampleBlock read; std::string identity;
        for (unsigned i = 0; i < count; ++i) {
            assert(j.peek(read, identity) && read.sequence == i); assert(j.acknowledge(i));
        }
        assert(!j.peek(read, identity)); // Full must not overwrite unacknowledged data.
    }
    {
        const auto path = root + "/torn";
        { FlashJournal j; assert(j.begin(path.c_str(), 1024 * 1024));
          assert(j.append(sample(7), meta)); assert(j.append(sample(8), meta)); }
        auto file = first(path); fs::resize_file(file, fs::file_size(file) - 20);
        FlashJournal recovered; assert(recovered.begin(path.c_str(), 1024 * 1024));
        assert(recovered.append(sample(9), "new-session"));
        SampleBlock read; std::string identity;
        assert(recovered.peek(read, identity) && read.sequence == 7); assert(recovered.acknowledge(7));
        assert(recovered.peek(read, identity) && read.sequence == 9); assert(recovered.acknowledge(9));
        assert(!recovered.peek(read, identity) && !recovered.failed());
        assert(fs::exists(file.string() + ".incomplete")); // Torn evidence is retained.
    }
    {
        const auto path = root + "/corrupt";
        { FlashJournal j; assert(j.begin(path.c_str(), 1024 * 1024)); assert(j.append(sample(4), meta)); }
        auto file = first(path);
        std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
        stream.seekp(-1, std::ios::end); stream.put('x'); stream.close();
        FlashJournal recovered; assert(recovered.begin(path.c_str(), 1024 * 1024));
        SampleBlock read; std::string identity;
        assert(!recovered.peek(read, identity) && recovered.failed());
        assert(fs::exists(file)); assert(!recovered.acknowledge(4));
    }

    for (size_t cut : {size_t(0), size_t(5), size_t(20), size_t(40), size_t(100)}) {
        const auto path = root + "/partial-" + std::to_string(cut);
        { FlashJournal j; assert(j.begin(path.c_str(), 1024 * 1024)); assert(j.append(sample(1), meta)); }
        auto file = first(path); fs::resize_file(file, cut);
        {
            FlashJournal j; assert(j.begin(path.c_str(), 1024 * 1024));
            assert(j.append(sample(2), "next")); SampleBlock read; std::string identity;
            assert(j.peek(read, identity) && read.sequence == 2 && !j.failed());
            assert(j.acknowledge(2)); assert(j.incompleteRecovered() == 1);
        }
        const auto evidence = file.string() + ".incomplete";
        assert(fs::exists(evidence) && fs::file_size(evidence) == cut);
        FlashJournal again; assert(again.begin(path.c_str(), 1024 * 1024));
        assert(again.append(sample(3), "third"));
        assert(fs::file_size(evidence) == cut); // Never reuse an interrupted file's identity.
    }
    fs::remove_all(root);
    std::cout << "Flash journal restart, metadata, ACK, capacity, torn append and corruption passed\n";
}
