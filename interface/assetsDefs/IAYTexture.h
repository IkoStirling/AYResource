#pragma once
#include "IAYResource.h"
#include "AYIntermediateAsset.h"
#include "AYMath/MathTypes.h"
#include <cstdint>

namespace ayt::resource
{

// ===== 纹理过滤模式 =====
enum class TextureFilter : UInt8 {
    None = 0,
    Point,      // Nearest neighbor
    Bilinear,
    Trilinear,
    Anisotropic,
};

// ===== 纹理包裹模式 =====
enum class TextureWrap : UInt8 {
    Repeat = 0,
    Clamp,
    Mirror,
    Border,
};

// ===== 纹理采样器配置 =====
struct TextureSampler {
    TextureFilter minFilter = TextureFilter::Bilinear;
    TextureFilter magFilter = TextureFilter::Bilinear;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    TextureWrap wrapW = TextureWrap::Repeat;
    Float32 anisotropy = 1.0f;
    Bool sRGB = false;  // 是否使用 sRGB 色彩空间
    Float32 lodBias = 0.0f;
};

// ===== ITexture — 纹理资源接口 =====
class ITexture : public IResource {
public:
    virtual ~ITexture() = default;

    // ===== Dimensions =====
    virtual UInt32 getWidth() const = 0;
    virtual UInt32 getHeight() const = 0;
    virtual ayt::math::FVector2 getTexelSize() const = 0;  // {1/width, 1/height}

    // ===== Format =====
    virtual TextureFormat getFormat() const = 0;

    // ===== Mipmap =====
    virtual UInt32 getMipmapCount() const = 0;
    virtual const UInt8* getMipmapData(UInt32 mipLevel = 0) const = 0;
    virtual UInt32 getMipmapSize(UInt32 mipLevel = 0) const = 0;

    // ===== Sampler config =====
    virtual const TextureSampler& getSampler() const = 0;
    virtual void setSampler(const TextureSampler& sampler) = 0;

    // ===== Total size =====
    virtual size_t sizeInBytes() const = 0;

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x48545841; // 'AYTX'
};

} // namespace ayt::resource