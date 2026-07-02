#pragma once
#include "IAYResource.h"
#include <string>

namespace ayt::resource
{

// ============================================================
// IScript - 脚本资源接口
// ============================================================
class IScript : public IResource {
public:
    ~IScript() override = default;

    // ===== Script Info =====
    virtual const char* getName() const = 0;
    virtual const char* getLanguage() const = 0; // "lua", "python", "javascript"
    virtual const char* getSource() const = 0;

    // ===== Constants =====
    static constexpr UInt32 MAGIC = 0x43415941; // 'AYSC' little-endian
    static constexpr UInt32 VERSION = 1;
};

} // namespace ayt::resource