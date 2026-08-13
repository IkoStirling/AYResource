#include "Converter/AudioDecoder.h"

#include <ayio/File.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ----- minimp3 (vcpkg header-only) -----
#define MINIMP3_IMPLEMENTATION
#include <minimp3/minimp3.h>
#include <minimp3/minimp3_ex.h>

// ----- stb_vorbis (vcpkg Stb: implement in this TU only) -----
#include <stb_vorbis.c>

namespace ayt::resource
{
namespace {

std::string toLower(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool readEntireFile(const std::string& path, std::vector<uint8_t>& out)
{
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }
    const size_t n = file.size();
    out.resize(n);
    if (n == 0) {
        return true;
    }
    return file.read(out.data(), n) == n;
}

#pragma pack(push, 1)
struct WavRiff {
    uint32_t riffMagic;
    uint32_t fileSize;
    uint32_t waveMagic;
};
struct WavFmt {
    uint32_t chunkMagic;
    uint32_t chunkSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
struct WavChunk {
    uint32_t magic;
    uint32_t size;
};
#pragma pack(pop)

bool decodeWavMemory(const uint8_t* data, size_t size, PcmBuffer& out)
{
    out = {};
    if (size < sizeof(WavRiff) + sizeof(WavFmt)) {
        return false;
    }

    const auto* riff = reinterpret_cast<const WavRiff*>(data);
    if (riff->riffMagic != 0x46464952u || riff->waveMagic != 0x45564157u) {
        return false;
    }

    size_t pos = sizeof(WavRiff);
    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    const uint8_t* pcmPtr = nullptr;
    uint32_t pcmBytes = 0;

    while (pos + sizeof(WavChunk) <= size) {
        WavChunk chunk{};
        std::memcpy(&chunk, data + pos, sizeof(chunk));
        pos += sizeof(WavChunk);
        if (pos + chunk.size > size) {
            return false;
        }

        if (chunk.magic == 0x20746D66u) { // 'fmt '
            if (chunk.size < 16) {
                return false;
            }
            std::memcpy(&audioFormat, data + pos, 2);
            std::memcpy(&channels, data + pos + 2, 2);
            std::memcpy(&sampleRate, data + pos + 4, 4);
            std::memcpy(&bitsPerSample, data + pos + 14, 2);
        } else if (chunk.magic == 0x61746164u) { // 'data'
            pcmPtr = data + pos;
            pcmBytes = chunk.size;
        }

        pos += chunk.size + (chunk.size & 1u); // word align
    }

    if (pcmPtr == nullptr || pcmBytes == 0 || channels == 0 || sampleRate == 0) {
        return false;
    }
    // PCM (1) or IEEE float (3). We only accept integer PCM here; float WAV → fail for v1.
    if (audioFormat != 1) {
        return false;
    }
    if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) {
        return false;
    }

    const uint32_t bytesPerSample = bitsPerSample / 8u;
    const uint32_t frameSize = static_cast<uint32_t>(channels) * bytesPerSample;
    if (frameSize == 0 || (pcmBytes % frameSize) != 0) {
        return false;
    }
    const uint64_t frames = pcmBytes / frameSize;

    // Normalize to interleaved S16.
    out.sampleRate = sampleRate;
    out.channels = channels;
    out.bitsPerSample = 16;
    out.frameCount = frames;
    out.bytes.resize(static_cast<size_t>(frames) * channels * 2u);

    auto* dst = reinterpret_cast<int16_t*>(out.bytes.data());
    for (uint64_t i = 0; i < frames * channels; ++i) {
        const uint8_t* s = pcmPtr + static_cast<size_t>(i) * bytesPerSample;
        int32_t sample = 0;
        if (bitsPerSample == 8) {
            sample = (static_cast<int32_t>(s[0]) - 128) << 8;
        } else if (bitsPerSample == 16) {
            sample = static_cast<int16_t>(s[0] | (s[1] << 8));
        } else if (bitsPerSample == 24) {
            sample = static_cast<int32_t>(s[0] | (s[1] << 8) | (s[2] << 16));
            if (sample & 0x800000) {
                sample |= ~0xFFFFFF;
            }
            sample >>= 8;
        } else { // 32-bit int PCM
            int32_t v = 0;
            std::memcpy(&v, s, 4);
            sample = v >> 16;
        }
        if (sample > 32767) {
            sample = 32767;
        }
        if (sample < -32768) {
            sample = -32768;
        }
        dst[i] = static_cast<int16_t>(sample);
    }
    return true;
}

bool decodeMp3Memory(const uint8_t* data, size_t size, PcmBuffer& out)
{
    out = {};
    mp3dec_t dec;
    mp3dec_init(&dec);

    mp3dec_file_info_t info{};
    // minimp3_ex: decode whole buffer to s16 interleaved.
    const int err = mp3dec_load_buf(&dec, data, size, &info, nullptr, nullptr);
    if (err != 0 || info.buffer == nullptr || info.samples == 0) {
        if (info.buffer) {
            free(info.buffer);
        }
        return false;
    }

    out.sampleRate = static_cast<uint32_t>(info.hz);
    out.channels = static_cast<uint16_t>(info.channels);
    out.bitsPerSample = 16;
    // info.samples = total PCM samples (all channels).
    if (out.channels == 0) {
        free(info.buffer);
        return false;
    }
    out.frameCount = static_cast<uint64_t>(info.samples / out.channels);
    const size_t bytes = static_cast<size_t>(info.samples) * sizeof(mp3d_sample_t);
    out.bytes.resize(bytes);
    std::memcpy(out.bytes.data(), info.buffer, bytes);
    free(info.buffer);
    return true;
}

bool decodeOggMemory(const uint8_t* data, size_t size, PcmBuffer& out)
{
    out = {};
    int channels = 0;
    int sampleRate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(
        const_cast<unsigned char*>(data),
        static_cast<int>(size),
        &channels,
        &sampleRate,
        &pcm);
    if (frames <= 0 || pcm == nullptr || channels <= 0 || sampleRate <= 0) {
        if (pcm) {
            free(pcm);
        }
        return false;
    }

    out.sampleRate = static_cast<uint32_t>(sampleRate);
    out.channels = static_cast<uint16_t>(channels);
    out.bitsPerSample = 16;
    out.frameCount = static_cast<uint64_t>(frames);
    const size_t bytes = static_cast<size_t>(frames) * static_cast<size_t>(channels) * sizeof(short);
    out.bytes.resize(bytes);
    std::memcpy(out.bytes.data(), pcm, bytes);
    free(pcm);
    return true;
}

} // namespace

std::string audioFileExtension(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return {};
    }
    return toLower(path.substr(dot + 1));
}

bool isLooseAudioExtension(const std::string& extLowerNoDot)
{
    return extLowerNoDot == "wav" || extLowerNoDot == "mp3" || extLowerNoDot == "ogg";
}

bool isCookedAudioExtension(const std::string& extLowerNoDot)
{
    return extLowerNoDot == "ayaudio";
}

bool decodeAudioFile(const std::string& path, PcmBuffer& out)
{
    out = {};
    std::vector<uint8_t> fileBytes;
    if (!readEntireFile(path, fileBytes) || fileBytes.empty()) {
        return false;
    }

    const std::string ext = audioFileExtension(path);
    if (ext == "wav") {
        return decodeWavMemory(fileBytes.data(), fileBytes.size(), out);
    }
    if (ext == "mp3") {
        return decodeMp3Memory(fileBytes.data(), fileBytes.size(), out);
    }
    if (ext == "ogg") {
        return decodeOggMemory(fileBytes.data(), fileBytes.size(), out);
    }
    return false;
}

} // namespace ayt::resource
