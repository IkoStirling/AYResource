#pragma once
#include "IAYAudio.h"
#include "IAYResourceLoader.h"
#include <vector>
#include <string>
#include <aymath/MathTypes.h>
#include <ayio/File.h>

namespace ayt::resource
{

// ===== Audio — IAudio 实现类 =====
class Audio : public IAudio {
    friend class AudioConverter;

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
