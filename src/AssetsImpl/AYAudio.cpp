#include "AYAudio.h"
#include <AYMathTypes.h>
#include <AYMathUtils.h>
#include <AYFile.h>
#include <cstring>
#include <cstdio>

namespace ayt::resource
{

// ===== 常量 =====

// ===== Audio 二进制头 =====
#pragma pack(push, 1)
struct AudioBinaryHeader {
    UInt32 magic;           // 'AYAU' = 0x55415941
    UInt32 version;        // 版本 = 1
    FGuid guid;            // 资源唯一标识 (16 bytes)
    UInt32 nameLength;     // 名称长度
    UInt32 sampleRate;     // 采样率
    UInt32 channels;       // 通道数 (1=mono, 2=stereo)
    UInt32 bitsPerSample;  // 采样位数 (8/16/24/32)
    UInt64 sampleCount;    // 样本数量
    UInt32 dataSize;      // PCM 数据大小
};
#pragma pack(pop)

// ===== Audio =====

Audio::Audio() = default;

void Audio::clear() {
    _data.clear();
    _name.clear();
    _sampleRate = 44100;
    _channels = 1;
    _bitsPerSample = 16;
    _sampleCount = 0;
}

bool Audio::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t Audio::sizeInBytes() const {
    return sizeof(Audio) + _data.size();
}

Float32 Audio::getDuration() const {
    if (_sampleRate == 0 || _channels == 0) {
        return 0.0f;
    }
    return static_cast<Float32>(_sampleCount) / static_cast<Float32>(_sampleRate * _channels);
}

bool Audio::load(const std::string& path) {
    _path = path;

    // 从路径提取名称
    size_t lastSlash = _path.find_last_of("/\\");
    size_t lastDot = _path.find_last_of('.');
    if (lastDot != std::string::npos && lastSlash != std::string::npos) {
        _name = _path.substr(lastSlash + 1, lastDot - lastSlash - 1);
    } else if (lastDot != std::string::npos) {
        _name = _path.substr(0, lastDot);
    } else {
        _name = _path;
    }

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(AudioBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Audio::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(AudioBinaryHeader)) {
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const AudioBinaryHeader* header = reinterpret_cast<const AudioBinaryHeader*>(ptr);

    // 验证 magic 和 version
    if (header->magic != IAudio::MAGIC || header->version != IAudio::VERSION) {
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取基本属性
    _sampleRate = header->sampleRate;
    _channels = header->channels;
    _bitsPerSample = header->bitsPerSample;
    _sampleCount = header->sampleCount;

    // 读取 name
    if (header->nameLength > 0) {
        if (size < sizeof(AudioBinaryHeader) + header->nameLength) {
            return false;
        }
        _name.assign(reinterpret_cast<const char*>(ptr + sizeof(AudioBinaryHeader)), header->nameLength);
    } else {
        _name.clear();
    }

    // 读取 PCM 数据
    size_t dataOffset = sizeof(AudioBinaryHeader) + header->nameLength;
    if (header->dataSize > 0) {
        if (size < dataOffset + header->dataSize) {
            return false;
        }
        _data.resize(header->dataSize);
        std::memcpy(_data.data(), ptr + dataOffset, header->dataSize);
    }

    _loaded = true;
    return true;
}

bool Audio::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t nameLen = _name.size();
    size_t totalSize = sizeof(AudioBinaryHeader) + nameLen + _data.size();

    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Custom Header
    AudioBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = IAudio::MAGIC;
    header.version = IAudio::VERSION;
    header.guid = _guid;
    header.nameLength = static_cast<UInt32>(nameLen);
    header.sampleRate = _sampleRate;
    header.channels = _channels;
    header.bitsPerSample = _bitsPerSample;
    header.sampleCount = _sampleCount;
    header.dataSize = static_cast<UInt32>(_data.size());

    std::memcpy(ptr, &header, sizeof(header));

    // 写入 name
    if (nameLen > 0) {
        std::memcpy(ptr + sizeof(AudioBinaryHeader), _name.data(), nameLen);
    }

    // 写入 PCM 数据
    if (!_data.empty()) {
        std::memcpy(ptr + sizeof(AudioBinaryHeader) + nameLen, _data.data(), _data.size());
    }

    return true;
}

// ===== 创建测试数据 =====

void Audio::createSineWave(Float32 frequency, Float32 duration) {
    clear();

    _sampleRate = 44100;
    _channels = 1;
    _bitsPerSample = 16;
    _name = "sine_wave";

    Float32 sampleDuration = duration;
    _sampleCount = static_cast<UInt64>(_sampleRate * sampleDuration);
    UInt32 byteRate = _sampleRate * _channels * (_bitsPerSample / 8);
    UInt32 dataSize = byteRate * static_cast<UInt32>(sampleDuration);
    _data.resize(dataSize);

    Float32 amplitude = 32000.0f; // 16-bit PCM 的典型振幅
    Float32 angularFreq = 2.0f * MATH_PI * frequency;

    Int16* samples = reinterpret_cast<Int16*>(_data.data());
    UInt64 numSamples = static_cast<UInt64>(_sampleRate * sampleDuration);

    for (UInt64 i = 0; i < numSamples; i++) {
        Float32 t = static_cast<Float32>(i) / static_cast<Float32>(_sampleRate);
        Float32 value = amplitude * ayt::math::sin(angularFreq * t);
        samples[i] = static_cast<Int16>(value);
    }

    _loaded = true;
}

void Audio::createSilence(UInt64 sampleCount) {
    clear();

    _sampleRate = 44100;
    _channels = 1;
    _bitsPerSample = 16;
    _sampleCount = sampleCount;
    _name = "silence";

    UInt32 dataSize = static_cast<UInt32>(sampleCount * sizeof(Int16));
    _data.resize(dataSize, 0);

    _loaded = true;
}

} // namespace ayt::resource