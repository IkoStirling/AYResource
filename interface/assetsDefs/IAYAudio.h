#pragma once
#include "IAYResource.h"

namespace ayt::resource
{

// ===== IAudio —音频资源接口 =====
class IAudio : public IResource {
public:
    virtual ~IAudio() = default;

    // ===== Audio info =====
    virtual const char* getName() const = 0;
    virtual UInt32 getSampleRate() const = 0;
    virtual UInt32 getChannels() const = 0;
    virtual UInt32 getBitsPerSample() const = 0;
    virtual UInt64 getSampleCount() const = 0;
    virtual const UInt8* getData() const = 0;

    // ===== Computed =====
    // Duration in seconds
    virtual Float32 getDuration() const = 0;
    // Data size in bytes
    virtual UInt32 getDataSize() const = 0;

    // ===== Helpers =====
    Bool isStereo() const { return getChannels() == 2; }
    Bool isMono() const { return getChannels() == 1; }

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x55415941; // 'AYAU' in little-endian
};

} // namespace ayt::resource