#pragma once
#include "IAYTexture.h"
#include "IAYResource.h"
#include "aymath/MathTypes.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace ayt::resource
{

// ===== Texture — ITexture 实现类 =====
class Texture : public ITexture {
    friend class TextureConverter;

public:
    Texture();
    virtual ~Texture() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;

    // ===== ITexture =====
    UInt32 getWidth() const override { return _width; }
    UInt32 getHeight() const override { return _height; }
    ayt::math::FVector2 getTexelSize() const override { return ayt::math::FVector2(1.0f / _width, 1.0f / _height); }
    TextureFormat getFormat() const override { return _format; }
    UInt32 getMipmapCount() const override { return static_cast<UInt32>(_mipOffsets.size()); }
    const UInt8* getMipmapData(UInt32 mipLevel = 0) const override;
    UInt32 getMipmapSize(UInt32 mipLevel = 0) const override;

    // ===== Sampler =====
    const TextureSampler& getSampler() const override { return _sampler; }
    void setSampler(const TextureSampler& sampler) override { _sampler = sampler; }

    size_t sizeInBytes() const override;

    // ===== 从二进制加载/保存 =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== 创建测试数据 =====
    void createSolidColor(UInt32 width, UInt32 height, UInt32 r, UInt32 g, UInt32 b, UInt32 a = 255);
    void createCheckerboard(UInt32 width, UInt32 height, UInt32 checkSize = 8);

    // ===== 属性访问 (用于 Converter/Loader) =====
    UInt32 _width = 0;
    UInt32 _height = 0;
    TextureFormat _format = TextureFormat::RGBA8;
    UInt32 _mipmapCount = 1;

public:
    static UInt32 computeMipSize(UInt32 width, UInt32 height, TextureFormat format);

private:
    void clear();

    FGuid _guid;  // 资源唯一标识
    std::vector<UInt8> _imageData;
    std::vector<UInt32> _mipOffsets;
    std::vector<UInt32> _mipSizes;
    TextureSampler _sampler;
    std::string _path;
};

// ===== Inline implementations =====

inline const UInt8* Texture::getMipmapData(UInt32 mipLevel) const {
    if (mipLevel >= _mipOffsets.size()) {
        return nullptr;
    }
    return _imageData.data() + _mipOffsets[mipLevel];
}

inline UInt32 Texture::getMipmapSize(UInt32 mipLevel) const {
    if (mipLevel >= _mipSizes.size()) {
        return 0;
    }
    return _mipSizes[mipLevel];
}

} // namespace ayt::resource
