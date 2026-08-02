#include "Converter\AudioConverter.h"
#include "Loader\AudioLoader.h"
#include <ayio/File.h>
#include <aystorage/Guid.h>
#include <cstring>

namespace ayt::resource
{

// ===== WAV 文件头 =====
#pragma pack(push, 1)
struct WAVFileHeader {
    UInt32 riffMagic;        // 'RIFF' = 0x46464952
    UInt32 fileSize; // 文件大小 - 8
    UInt32 waveMagic;        // 'WAVE' = 0x45564157
};

struct WAVFormatChunk {
    UInt32 chunkMagic;       // 'fmt ' = 0x20746D66
    UInt32 chunkSize;        // 块大小 (16 for PCM)
    UInt16 audioFormat;     // 音频格式 (1 = PCM)
    UInt16 numChannels;      // 通道数
    UInt32 sampleRate;       // 采样率
    UInt32 byteRate;         // 字节率 (sampleRate * numChannels * bitsPerSample/8)
    UInt16 blockAlign;       // 块对齐 (numChannels * bitsPerSample/8)
    UInt16 bitsPerSample;    // 每样本位数
};

struct WAVDataChunk {
    UInt32 chunkMagic;      // 'data' = 0x61746164
    UInt32 dataSize;        // 数据大小
};
#pragma pack(pop)

AudioConverter::AudioConverter() = default;

AudioConverter::AudioConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void AudioConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void AudioConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

// ===== 工具函数 =====
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

static std::string getExtension(const std::string& path) {
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) {
        return "";
    }
    return path.substr(dotPos + 1);
}

// ===== 加载 WAV 文件 =====
static bool loadWAV(const std::string& path, std::vector<UInt8>& audioData,
                   UInt32& sampleRate, UInt32& channels, UInt32& bitsPerSample, UInt64& sampleCount) {
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    WAVFileHeader fileHeader;
    if (file.read(&fileHeader, sizeof(fileHeader)) != sizeof(fileHeader)) {
        return false;
    }

    // 验证 RIFF/WAVE magic
    if (fileHeader.riffMagic != 0x46464952 || fileHeader.waveMagic != 0x45564157) {
        return false;
    }

    // 读取 fmt chunk
    WAVFormatChunk fmtChunk;
    if (file.read(&fmtChunk, sizeof(fmtChunk)) != sizeof(fmtChunk)) {
        return false;
    }

    if (fmtChunk.chunkMagic != 0x20746D66) { // 'fmt '
        return false;
    }

    // 只支持 PCM 格式
    if (fmtChunk.audioFormat != 1) {
        return false;
    }

    sampleRate = fmtChunk.sampleRate;
    channels = fmtChunk.numChannels;
    bitsPerSample = fmtChunk.bitsPerSample;

    // 读取 data chunk
    WAVDataChunk dataChunk;
    if (file.read(&dataChunk, sizeof(dataChunk)) != sizeof(dataChunk)) {
        return false;
    }

    if (dataChunk.chunkMagic != 0x61746164) { // 'data'
        return false;
    }

    // 读取音频数据
    audioData.resize(dataChunk.dataSize);
    if (file.read(audioData.data(), dataChunk.dataSize) != dataChunk.dataSize) {
        return false;
    }

    // 计算样本数量
    UInt32 bytesPerSample = bitsPerSample / 8;
    UInt32 frameSize = channels * bytesPerSample;
    sampleCount = (frameSize > 0) ? dataChunk.dataSize / frameSize : 0;

    return true;
}

ConversionResult AudioConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        return result;
    }

    std::string ext = getExtension(sourcePath);
    for (auto& c : ext) c = static_cast<char>(tolower(c));

    // 加载源音频
    std::vector<UInt8> audioData;
    UInt32 sampleRate = 44100;
    UInt32 channels = 1;
    UInt32 bitsPerSample = 16;
    UInt64 sampleCount = 0;

    bool loaded = false;
    if (ext == "wav") {
        loaded = loadWAV(sourcePath, audioData, sampleRate, channels, bitsPerSample, sampleCount);
    }
    // OGG 支持需要 OggVorbis 库，此处简化处理
    // 如需支持，可集成 libvorbis 或使用 stb_vorbis

    if (!loaded) {
        return result;
    }

    // 创建 Audio
    Audio audio;
    audio._name = getBaseName(sourcePath);
    audio._sampleRate = outputSampleRate > 0 ? outputSampleRate : sampleRate;
    audio._channels = outputChannels > 0 ? outputChannels : channels;
    audio._bitsPerSample = outputBitsPerSample > 0 ? outputBitsPerSample : bitsPerSample;
    audio._sampleCount = sampleCount;
    audio._data = audioData;

    audio._loaded = true;

    // 计算 GUID
    lastGuid = ayt::storage::Guid::computeFromData(audio._data.data(), audio._data.size());
    audio.setGuid(lastGuid);

    // 保存到二进制数据
    std::vector<UInt8> binaryData;
    if (!audio.saveToBinary(binaryData)) {
        return result;
    }

    // 生成输出文件名: audio/{name}.ayaudio
    std::string baseName = getBaseName(sourcePath);
    std::string outputFileName = baseName + ".ayaudio";
    std::string virtualPath = "audio/" + outputFileName;

    // 写入输出目录
    if (!outputDir.empty()) {
        std::string fullPath = outputDir + "/" + virtualPath;
        if (!ayt::io::File::exists(fullPath)) {
            if (!writeFile(fullPath, binaryData.data(), binaryData.size())) {
                return result;
            }
        }
    }

    // 构建资源信息
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

        // 创建 Audio
        Audio audio;
        audio._name = name;
        audio._sampleRate = outputSampleRate > 0 ? outputSampleRate : static_cast<UInt32>(audioData.sampleRate);
        audio._channels = outputChannels > 0 ? outputChannels : static_cast<UInt32>(audioData.channels);
        audio._bitsPerSample = outputBitsPerSample > 0 ? outputBitsPerSample : static_cast<UInt32>(audioData.bitsPerSample);
        audio._sampleCount = audioData.audioData.size() / ((audio._bitsPerSample / 8) * audio._channels);
        audio._data = audioData.audioData;

        audio._loaded = true;

        // 计算 GUID
        lastGuid = ayt::storage::Guid::computeFromData(audio._data.data(), audio._data.size());
        audio.setGuid(lastGuid);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        if (!audio.saveToBinary(binaryData)) {
            continue;
        }

        // 写入输出目录
        if (!outputDir.empty()) {
            std::string fullPath = outputDir + "/" + virtualPath;
            if (!ayt::io::File::exists(fullPath)) {
                writeFile(fullPath, binaryData.data(), binaryData.size());
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