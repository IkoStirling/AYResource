#pragma once
#include "AYResource/IResource.h"
#include <cstdint>

namespace ayt::resource
{

// ===== IShader — 着色器资源接口 =====
class IShader : public IResource {
public:
    virtual ~IShader() = default;

    // ===== Shader Info =====
    virtual const char* getName() const = 0;
    virtual const char* getSource() const = 0;
    virtual const char* getEntryPoint() const = 0;
    virtual const char* getProfile() const = 0;

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    // MAGIC: 'AYSH' = 0x48 0x53 0x59 0x41 in bytes
    static constexpr UInt32 MAGIC = 0x48595341;
    static constexpr UInt32 FILE_MAGIC = 0x48595341;
};

} // namespace ayt::resource