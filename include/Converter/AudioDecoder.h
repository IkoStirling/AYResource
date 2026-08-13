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

// Decode common formats to interleaved S16 PCM (full-file).
bool decodeAudioFile(const std::string& path, PcmBuffer& out);

// Linear resample interleaved S16 PCM to targetRate (channels preserved).
// No-op copy when rates match. Returns false on invalid input.
bool resamplePcmS16(const PcmBuffer& in, uint32_t targetRate, PcmBuffer& out);

// Engine / cook default sample rate (AYAudio design §3.2).
constexpr uint32_t kEngineAudioSampleRate = 48000;

// Incremental stream decoder for long BGM (decode → float frames → AYAudio
// streamPush). Opaque handle; not thread-safe — drive from one loader thread.
struct AudioStreamDecoder;

AudioStreamDecoder* openAudioStreamDecoder(const std::string& path);
// Decode up to maxFrames interleaved float32 frames into outInterleavedF32
// (channels * maxFrames floats). Returns frames written; 0 = EOS or error.
// Sets *endOfStream when no more audio remains.
uint32_t decodeAudioStreamFrames(AudioStreamDecoder* dec,
                                 float* outInterleavedF32,
                                 uint32_t maxFrames,
                                 bool* endOfStream);
uint16_t audioStreamDecoderChannels(const AudioStreamDecoder* dec);
uint32_t audioStreamDecoderSampleRate(const AudioStreamDecoder* dec); // source rate
void closeAudioStreamDecoder(AudioStreamDecoder* dec);

// Extension helpers (lowercase, no leading dot). Empty if none.
std::string audioFileExtension(const std::string& path);

bool isLooseAudioExtension(const std::string& extLowerNoDot); // wav/mp3/ogg
bool isCookedAudioExtension(const std::string& extLowerNoDot); // ayaudio

} // namespace ayt::resource
