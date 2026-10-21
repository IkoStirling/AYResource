#pragma once
#include "AYResource/IResource.h"
#include "AYResource/IntermediateAsset.h"
#include "AYResource/TextureSampling.h"
#include "AYMath/MathTypes.h"
#include <cstdint>

namespace ayt::resource
{

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
    /// Updates this CPU resource only. Saving or publishing/reloading resident
    /// GPU consumers is an explicit authoring/Host operation, not a side effect.
    virtual void setSampler(const TextureSampler& sampler) = 0;

    // ===== Total size =====
    virtual size_t sizeInBytes() const = 0;

    // ===== Constants =====
    /// .txr v2 stores the sampler with the encoded mip chain. Readers also
    /// accept v1 assets using the historical default sampler. GPU upload and
    /// device-specific sampling fallbacks are owned by AYRenderer.
    static constexpr UInt32 VERSION = 2;
    static constexpr UInt32 MAGIC = 0x48545841; // Historical numeric magic; keep compatible with v1.
};

} // namespace ayt::resource
