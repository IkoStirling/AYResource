#pragma once
#include "IAYVideo.h"
#include <memory>
#include <vector>
#include <string>

namespace ayt::resource
{

// ===== Video — IVideo 实现类 =====
class Video : public IVideo {
    // F2.1: removed `friend class VideoConverter;`. Converter uses the
    // public setters below (setName / setFrameInfo / setFrameData) to
    // populate this asset without touching private fields.

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
    // F2.1: aggregates the wide fields + frameRate so the converter
    // doesn't write to each public field directly. duration is computed
    // here from frameCount/frameRate so converters don't have to repeat
    // the formula.
    void setFrameInfo(UInt32 width, UInt32 height, float frameRate,
                      UInt32 frameCount, UInt32 frameSize) {
        _width = width;
        _height = height;
        _frameRate = frameRate;
        _frameCount = frameCount;
        _frameSize = frameSize;
        _duration = (frameRate > 0.0f)
            ? static_cast<float>(frameCount) / frameRate
            : 0.0f;
    }
    void setFrameData(std::vector<UInt8>&& bytes) { _frameData = std::move(bytes); }
    void setFrameData(const std::vector<UInt8>& bytes) { _frameData = bytes; }
    void setFrameData(const UInt8* bytes, size_t n) {
        _frameData.assign(bytes, bytes + n);
    }
    // F2.1: read-only view of the full frame buffer (for callers that
    // need to hash the asset bytes end-to-end, e.g. the converter GUID).
    const UInt8* getFrameDataBytes() const { return _frameData.data(); }
    size_t getFrameDataBytesSize() const { return _frameData.size(); }

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