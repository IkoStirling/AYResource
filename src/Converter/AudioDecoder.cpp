#include "Converter/AudioDecoder.h"

#include <AYIO/File.h>

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

bool resamplePcmS16(const PcmBuffer& in, uint32_t targetRate, PcmBuffer& out)
{
    out = {};
    if (in.empty() || in.bitsPerSample != 16 || in.channels == 0 || in.sampleRate == 0
        || targetRate == 0) {
        return false;
    }
    if (in.sampleRate == targetRate) {
        out = in;
        return true;
    }

    const uint64_t inFrames = in.frameCount;
    const uint16_t ch = in.channels;
    const double ratio = static_cast<double>(targetRate) / static_cast<double>(in.sampleRate);
    const uint64_t outFrames = static_cast<uint64_t>(static_cast<double>(inFrames) * ratio + 0.5);
    if (outFrames == 0) {
        return false;
    }

    out.sampleRate = targetRate;
    out.channels = ch;
    out.bitsPerSample = 16;
    out.frameCount = outFrames;
    out.bytes.resize(static_cast<size_t>(outFrames) * ch * 2u);

    const auto* src = reinterpret_cast<const int16_t*>(in.bytes.data());
    auto* dst = reinterpret_cast<int16_t*>(out.bytes.data());

    for (uint64_t i = 0; i < outFrames; ++i) {
        const double srcPos = static_cast<double>(i) / ratio;
        uint64_t i0 = static_cast<uint64_t>(srcPos);
        if (i0 >= inFrames) {
            i0 = inFrames - 1;
        }
        uint64_t i1 = i0 + 1;
        if (i1 >= inFrames) {
            i1 = inFrames - 1;
        }
        const float t = static_cast<float>(srcPos - static_cast<double>(i0));
        for (uint16_t c = 0; c < ch; ++c) {
            const float a = static_cast<float>(src[i0 * ch + c]);
            const float b = static_cast<float>(src[i1 * ch + c]);
            const float s = a + (b - a) * t;
            int32_t v = static_cast<int32_t>(s < 0.f ? s - 0.5f : s + 0.5f);
            if (v > 32767) {
                v = 32767;
            }
            if (v < -32768) {
                v = -32768;
            }
            dst[i * ch + c] = static_cast<int16_t>(v);
        }
    }
    return true;
}

// ===== Incremental stream decoder =====

struct AudioStreamDecoder {
    enum class Kind : uint8_t { Wav, Mp3, Ogg };
    Kind kind = Kind::Wav;
    std::vector<uint8_t> fileBytes; // keep source alive for memory decoders
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    bool eof = false;

    // WAV cursor (S16 interleaved frames already normalized in buffer)
    PcmBuffer wavPcm;
    uint64_t wavFrameCursor = 0;

    // MP3 streaming
    mp3dec_ex_t mp3{};
    bool mp3Open = false;

    // OGG streaming
    stb_vorbis* vorbis = nullptr;
};

AudioStreamDecoder* openAudioStreamDecoder(const std::string& path)
{
    auto* dec = new AudioStreamDecoder();
    if (!readEntireFile(path, dec->fileBytes) || dec->fileBytes.empty()) {
        delete dec;
        return nullptr;
    }

    const std::string ext = audioFileExtension(path);
    if (ext == "wav") {
        if (!decodeWavMemory(dec->fileBytes.data(), dec->fileBytes.size(), dec->wavPcm)
            || dec->wavPcm.empty()) {
            delete dec;
            return nullptr;
        }
        dec->kind = AudioStreamDecoder::Kind::Wav;
        dec->channels = dec->wavPcm.channels;
        dec->sampleRate = dec->wavPcm.sampleRate;
        return dec;
    }
    if (ext == "mp3") {
        if (mp3dec_ex_open_buf(&dec->mp3, dec->fileBytes.data(), dec->fileBytes.size(),
                              MP3D_SEEK_TO_SAMPLE) != 0) {
            delete dec;
            return nullptr;
        }
        dec->mp3Open = true;
        dec->kind = AudioStreamDecoder::Kind::Mp3;
        dec->channels = static_cast<uint16_t>(dec->mp3.info.channels);
        dec->sampleRate = static_cast<uint32_t>(dec->mp3.info.hz);
        if (dec->channels == 0 || dec->sampleRate == 0) {
            mp3dec_ex_close(&dec->mp3);
            delete dec;
            return nullptr;
        }
        return dec;
    }
    if (ext == "ogg") {
        int err = 0;
        dec->vorbis = stb_vorbis_open_memory(
            dec->fileBytes.data(),
            static_cast<int>(dec->fileBytes.size()),
            &err,
            nullptr);
        if (dec->vorbis == nullptr) {
            delete dec;
            return nullptr;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(dec->vorbis);
        dec->kind = AudioStreamDecoder::Kind::Ogg;
        dec->channels = static_cast<uint16_t>(info.channels);
        dec->sampleRate = static_cast<uint32_t>(info.sample_rate);
        return dec;
    }

    delete dec;
    return nullptr;
}

uint16_t audioStreamDecoderChannels(const AudioStreamDecoder* dec)
{
    return dec ? dec->channels : 0;
}

uint32_t audioStreamDecoderSampleRate(const AudioStreamDecoder* dec)
{
    return dec ? dec->sampleRate : 0;
}

uint32_t decodeAudioStreamFrames(AudioStreamDecoder* dec,
                                 float* outInterleavedF32,
                                 uint32_t maxFrames,
                                 bool* endOfStream)
{
    if (endOfStream) {
        *endOfStream = false;
    }
    if (dec == nullptr || outInterleavedF32 == nullptr || maxFrames == 0 || dec->eof) {
        if (endOfStream && dec && dec->eof) {
            *endOfStream = true;
        }
        return 0;
    }

    uint32_t produced = 0;
    if (dec->kind == AudioStreamDecoder::Kind::Wav) {
        const auto* src = reinterpret_cast<const int16_t*>(dec->wavPcm.bytes.data());
        const uint16_t ch = dec->channels;
        while (produced < maxFrames && dec->wavFrameCursor < dec->wavPcm.frameCount) {
            for (uint16_t c = 0; c < ch; ++c) {
                const int16_t s = src[dec->wavFrameCursor * ch + c];
                outInterleavedF32[produced * ch + c] = static_cast<float>(s) / 32768.0f;
            }
            ++dec->wavFrameCursor;
            ++produced;
        }
        if (dec->wavFrameCursor >= dec->wavPcm.frameCount) {
            dec->eof = true;
            if (endOfStream) {
                *endOfStream = true;
            }
        }
        return produced;
    }

    if (dec->kind == AudioStreamDecoder::Kind::Mp3) {
        // mp3dec_ex_read returns number of *samples* (all channels).
        const size_t wantSamples = static_cast<size_t>(maxFrames) * dec->channels;
        std::vector<mp3d_sample_t> tmp(wantSamples);
        const size_t got = mp3dec_ex_read(&dec->mp3, tmp.data(), wantSamples);
        produced = static_cast<uint32_t>(got / dec->channels);
        for (uint32_t i = 0; i < produced * dec->channels; ++i) {
            outInterleavedF32[i] = static_cast<float>(tmp[i]) / 32768.0f;
        }
        if (got == 0 || got < wantSamples) {
            dec->eof = true;
            if (endOfStream) {
                *endOfStream = true;
            }
        }
        return produced;
    }

    if (dec->kind == AudioStreamDecoder::Kind::Ogg) {
        // stb returns samples per channel.
        const int got = stb_vorbis_get_samples_float_interleaved(
            dec->vorbis,
            dec->channels,
            outInterleavedF32,
            static_cast<int>(maxFrames * dec->channels));
        if (got < 0) {
            dec->eof = true;
            if (endOfStream) {
                *endOfStream = true;
            }
            return 0;
        }
        produced = static_cast<uint32_t>(got);
        if (got == 0) {
            dec->eof = true;
            if (endOfStream) {
                *endOfStream = true;
            }
        }
        return produced;
    }

    return 0;
}

void closeAudioStreamDecoder(AudioStreamDecoder* dec)
{
    if (dec == nullptr) {
        return;
    }
    if (dec->mp3Open) {
        mp3dec_ex_close(&dec->mp3);
        dec->mp3Open = false;
    }
    if (dec->vorbis) {
        stb_vorbis_close(dec->vorbis);
        dec->vorbis = nullptr;
    }
    delete dec;
}

} // namespace ayt::resource
