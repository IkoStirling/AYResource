#pragma once
#include "IAYResource.h"
#include <cstdint>

namespace ayt::resource
{

// ===== IVideo — 视频资源接口 =====
class IVideo : public IResource {
public:
    virtual ~IVideo() = default;

    // ===== Basic Info =====
    virtual const char* getName() const = 0;
    virtual UInt32 getWidth() const = 0;
    virtual UInt32 getHeight() const = 0;
    virtual float getDuration() const = 0;
    virtual float getFrameRate() const = 0;
    virtual UInt32 getFrameCount() const = 0;

    // ===== Frame Access =====
    virtual const UInt8* getFrameData(UInt32 frameIndex) const = 0;
    virtual UInt32 getFrameSize() const = 0;  // bytes per frame (width * height * 4 for RGBA)

    // ===== Total size =====
    virtual size_t sizeInBytes() const override = 0;

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x44415941; // 'AYVD' in little-endian
};

} // namespace ayt::resource