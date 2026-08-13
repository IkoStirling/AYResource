#pragma once
// AudioDecoder — decode common authoring formats to interleaved PCM (AYAudio design §2.3).
// PRIVATE to AYResource (Converter/). AYAudio never includes this header.

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::resource
{

struct PcmBuffer {
    std::vector<uint8_t> bytes; // interleaved PCM
    uint32_t sampleRate = 0;
    uint16_t channels = 0;      // 1 or 2
    uint16_t bitsPerSample = 16; // v1 decoder normalizes to S16
    uint64_t frameCount = 0;    // samples per channel

    bool empty() const { return bytes.empty() || frameCount == 0; }
};

// Returns true on success. On failure, out is cleared and false is returned.
bool decodeAudioFile(const std::string& path, PcmBuffer& out);

// Extension helpers (lowercase, no leading dot). Empty if none.
std::string audioFileExtension(const std::string& path);

bool isLooseAudioExtension(const std::string& extLowerNoDot); // wav/mp3/ogg
bool isCookedAudioExtension(const std::string& extLowerNoDot); // ayaudio

} // namespace ayt::resource
