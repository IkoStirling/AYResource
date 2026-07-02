#pragma once
#include "IAYVideo.h"
#include <memory>
#include <vector>
#include <string>

namespace ayt::resource
{

// ===== Video — IVideo 实现类 =====
class Video : public IVideo {
    friend class VideoConverter;

public:
    Video();
    virtual ~Video() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;

    // ===== IAYVideo =====
    const char* getName() const override { return _name.c_str(); }
    UInt32 getWidth() const override { return _width; }
    UInt32 getHeight() const override { return _height; }
    float getDuration() const override { return _duration; }
    float getFrameRate() const override { return _frameRate; }
    UInt32 getFrameCount() const override { return _frameCount; }
    const UInt8* getFrameData(UInt32 frameIndex) const override;
    UInt32 getFrameSize() const override { return _frameSize; }

    size_t sizeInBytes() const override;

    // ===== 二进制序列化 =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== 创建测试数据 =====
    void createTestVideo(UInt32 width, UInt32 height, UInt32 frameCount, float frameRate);

    // ===== Setters =====
    void setName(const std::string& name) { _name = name; }

    // ===== 属性访问 (用于 Converter/Loader) =====
    UInt32 _width = 0;
    UInt32 _height = 0;
    float _duration = 0.0f;
    float _frameRate = 30.0f;
    UInt32 _frameCount = 0;
    UInt32 _frameSize = 0;

private:
    void clear();

    FGuid _guid;  // 资源唯一标识
    std::string _name;
    std::vector<UInt8> _frameData;  // 扁平化存储: frame0 + frame1 + ...
    std::string _path;
};

// ===== Inline implementations =====

inline const UInt8* Video::getFrameData(UInt32 frameIndex) const {
    if (frameIndex >= _frameCount) {
        return nullptr;
    }
    return _frameData.data() + static_cast<size_t>(frameIndex) * _frameSize;
}

} // namespace ayt::resource