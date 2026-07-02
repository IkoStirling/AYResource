#pragma once
#include "IAYResource.h"
#include <cstdint>

namespace ayt::resource
{

// ===== IFontAsset — 字体资源接口 =====
class IFontAsset : public IResource {
public:
    virtual ~IFontAsset() = default;

    // ===== Font Info =====
    virtual const char* getName() const = 0;
    virtual UInt32 getFontSize() const = 0;
    virtual UInt32 getGlyphCount() const = 0;

    // ===== Glyph Info =====
    // 获取字形信息，codepoint 为 Unicode 码点
    // u,v,w,h 为该字形在 atlas 中的 UV 区域
    // advance 为字形宽度（用于字距调整）
    // 返回是否成功（如果该 codepoint 不存在）
    virtual bool getGlyphInfo(UInt32 codepoint, float& u, float& v,
                              float& w, float& h, float& advance) const = 0;

    // ===== Atlas =====
    virtual const UInt8* getAtlasData() const = 0;
    virtual UInt32 getAtlasWidth() const = 0;
    virtual UInt32 getAtlasHeight() const = 0;

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x4E465941; // 'AYFN' in little-endian
};

} // namespace ayt::resource