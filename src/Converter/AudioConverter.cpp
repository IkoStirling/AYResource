#include "Converter\AudioConverter.h"
#include "Converter\AudioDecoder.h"
#include "Loader\AudioLoader.h"
#include <AYIO/File.h>
#include <AYIO/Directory.h>
#include <AYStorage/Guid.h>
#include <cstring>

namespace ayt::resource
{

AudioConverter::AudioConverter() = default;

AudioConverter::AudioConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void AudioConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void AudioConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

static std::string getFileName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

static std::string getBaseName(const std::string& path) {
    std::string fileName = getFileName(path);
    size_t dotPos = fileName.find_last_of('.');
    if (dotPos == std::string::npos) {
        return fileName;
    }
    return fileName.substr(0, dotPos);
}

ConversionResult AudioConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        return result;
    }

    PcmBuffer pcm;
    if (!decodeAudioFile(sourcePath, pcm) || pcm.empty()) {
        return result;
    }

    const UInt32 targetRate = outputSampleRate > 0 ? outputSampleRate : kEngineAudioSampleRate;
    PcmBuffer cooked = pcm;
    if (pcm.sampleRate != targetRate) {
        if (!resamplePcmS16(pcm, targetRate, cooked) || cooked.empty()) {
            return result;
        }
    }

    const UInt32 useSr = cooked.sampleRate;
    const UInt32 useCh = cooked.channels;
    const UInt32 useBps = cooked.bitsPerSample;

    Audio audio;
    audio.setName(getBaseName(sourcePath));
    audio.setFormat(useSr, useCh, useBps, cooked.frameCount);
    audio.setData(cooked.bytes);
    audio.setLoaded(true);

    lastGuid = ayt::storage::Guid::computeFromData(audio.getDataBytes(), audio.getDataBytesSize());
    audio.setGuid(lastGuid);

    std::vector<UInt8> binaryData;
    if (!audio.saveToBinary(binaryData)) {
        return result;
    }

    std::string baseName = getBaseName(sourcePath);
    std::string outputFileName = baseName + ".ayaudio";
    std::string virtualPath = "audio/" + outputFileName;

    if (!outputDir.empty()) {
        std::string fullPath = outputDir + "/" + virtualPath;
        const std::string audioDir = outputDir + "/audio";
        (void)ayt::io::createDirectory(audioDir);
        if (!writeFile(fullPath, binaryData.data(), binaryData.size())) {
            return result;
        }
    }

    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Audio";
    res.size = static_cast<uint64_t>(binaryData.size());
    result.resources.push_back(res);

    return result;
}

std::vector<ConversionResult::ConvertedResource> AudioConverter::convertAll(
    const std::vector<AudioData>& audios,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (size_t i = 0; i < audios.size(); i++) {
        const auto& audioData = audios[i];
        std::string name = audioData.name.empty()
            ? baseName + "_" + std::to_string(i) + ".ayaudio"
            : baseName + "_" + audioData.name + ".ayaudio";

        std::string virtualPath = "audio/" + name;

        Audio audio;
        audio.setName(name);
        const UInt32 sr = outputSampleRate > 0 ? outputSampleRate : static_cast<UInt32>(audioData.sampleRate);
        const UInt32 ch = outputChannels > 0 ? outputChannels : static_cast<UInt32>(audioData.channels);
        const UInt32 bps = outputBitsPerSample > 0 ? outputBitsPerSample : static_cast<UInt32>(audioData.bitsPerSample);
        const UInt64 samples = audioData.audioData.size() / ((bps / 8) * ch);
        audio.setFormat(sr, ch, bps, samples);
        audio.setData(audioData.audioData);

        audio.setLoaded(true);

        lastGuid = ayt::storage::Guid::computeFromData(audio.getDataBytes(), audio.getDataBytesSize());
        audio.setGuid(lastGuid);

        std::vector<UInt8> binaryData;
        if (!audio.saveToBinary(binaryData)) {
            continue;
        }

        if (!outputDir.empty()) {
            std::string fullPath = outputDir + "/" + virtualPath;
            (void)ayt::io::createDirectory(outputDir + "/audio");
            if (!writeFile(fullPath, binaryData.data(), binaryData.size())) {
                continue;
            }
        }

        ConversionResult::ConvertedResource res;
        res.guid = lastGuid;
        res.path = virtualPath;
        res.type = "Audio";
        res.size = static_cast<uint64_t>(binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource
