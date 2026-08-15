#pragma once
#include "AYResource/assetsDefs/IAudio.h"
#include "AYResource/IResourceLoader.h"
#include <vector>
#include <string>
#include <AYMath/MathTypes.h>
#include <AYIO/File.h>

namespace ayt::resource
{

// ===== Audio — IAudio 实现类 =====
class Audio : public IAudio {
    // F2.1: removed `friend class AudioConverter;`. The converter now
    // uses the public setters below (setName / setFormat / setData) to
    // populate the asset without touching private members.

public:
    Audio();
    virtual ~Audio() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    // ===== IAYAudio =====
    const char* getName() const override { return _name.c_str(); }
    UInt32 getSampleRate() const override { return _sampleRate; }
    UInt32 getChannels() const override { return _channels; }
    UInt32 getBitsPerSample() const override { return _bitsPerSample; }
    UInt64 getSampleCount() const override { return _sampleCount; }
    const UInt8* getData() const override { return _data.data(); }

    Float32 getDuration() const override;
    UInt32 getDataSize() const override { return static_cast<UInt32>(_data.size()); }

    // ===== 从二进制加载/保存 =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== 创建测试数据 =====
    void createSineWave(Float32 frequency = 440.0f, Float32 duration = 1.0f);
    void createSilence(UInt64 sampleCount = 44100);

    // ===== F2.1: public setters for converter population =====
    void setName(const std::string& n) { _name = n; }
    void setFormat(UInt32 sampleRate, UInt32 channels, UInt32 bitsPerSample, UInt64 sampleCount) {
        _sampleRate = sampleRate;
        _channels = channels;
        _bitsPerSample = bitsPerSample;
        _sampleCount = sampleCount;
    }
    void setData(std::vector<UInt8>&& bytes) { _data = std::move(bytes); }
    void setData(const std::vector<UInt8>& bytes) { _data = bytes; }
    void setData(const UInt8* bytes, size_t n) {
        _data.assign(bytes, bytes + n);
    }
    // F2.1: read-only view of the raw PCM buffer (used by the converter
    // to compute the content hash without friend access).
    const UInt8* getDataBytes() const { return _data.data(); }
    size_t getDataBytesSize() const { return _data.size(); }

    // ===== 属性访问 (用于 Converter/Loader) =====
    std::string _name;
    UInt32 _sampleRate = 44100;
    UInt32 _channels = 1;
    UInt32 _bitsPerSample = 16;
    UInt64 _sampleCount = 0;

private:
    void clear();

    FGuid _guid;  // 资源唯一标识
    std::vector<UInt8> _data;
    std::string _path;
};

} // namespace ayt::resource
