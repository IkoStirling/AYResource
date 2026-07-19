#pragma once
#include "IAYResource.h"
#include "IAYFontAsset.h"
#include "IAYResourceLoader.h"
#include <aymath/MathTypes.h>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace ayt::resource
{

// ===== FontAsset — .ayfont 资源数据容器 =====
// 注意：这是 .ayfont 文件格式的数据容器，不包含渲染逻辑
// 渲染由 AYFont 模块的 FontRenderer负责

// ===== Glyph — 字形数据 =====
struct Glyph {
    UInt32 codepoint = 0;
    float u = 0.0f, v = 0.0f;      // UV 起始位置
    float w = 0.0f, h = 0.0f;     // 宽高
    float advance = 0.0f;           // 前进量
};

// ===== FontAsset — IFontAsset 实现类 =====
class FontAsset : public IFontAsset {
    friend class FontLoader;
    friend class FontConverter;

public:
    FontAsset();
    virtual ~FontAsset() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;

    // ===== IAYFont =====
    const char* getName() const override { return _name.c_str(); }
    UInt32 getFontSize() const override { return _fontSize; }
    UInt32 getGlyphCount() const override { return static_cast<UInt32>(_glyphs.size()); }

    bool getGlyphInfo(UInt32 codepoint, float& u, float& v,
                      float& w, float& h, float& advance) const override;

    const UInt8* getAtlasData() const override { return _atlasData.data(); }
    UInt32 getAtlasWidth() const override { return _atlasWidth; }
    UInt32 getAtlasHeight() const override { return _atlasHeight; }

    size_t sizeInBytes() const override;

    // ===== 二进制序列化 =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== Setters =====
    void setName(const std::string& name) { _name = name; }
    void setFontSize(UInt32 size) { _fontSize = size; }
    void setAtlasSize(UInt32 width, UInt32 height) { _atlasWidth = width; _atlasHeight = height; }

    // ===== 字形管理 =====
    void addGlyph(const Glyph& glyph);
    void setAtlasData(const UInt8* data, size_t size);

private:
    void clear();

    FGuid _guid;  // 资源唯一标识
    std::string _name;
    UInt32 _fontSize = 0;
    UInt32 _atlasWidth = 0;
    UInt32 _atlasHeight = 0;
    std::vector<Glyph> _glyphs;
    std::unordered_map<UInt32, UInt32> _glyphMap;  // codepoint → index
    std::vector<UInt8> _atlasData;
    std::string _path;
};

// ===== Inline implementations =====

inline bool FontAsset::getGlyphInfo(UInt32 codepoint, float& u, float& v,
                               float& w, float& h, float& advance) const {
    auto it = _glyphMap.find(codepoint);
    if (it == _glyphMap.end()) {
        return false;
    }
    const Glyph& g = _glyphs[it->second];
    u = g.u;
    v = g.v;
    w = g.w;
    h = g.h;
    advance = g.advance;
    return true;
}

} // namespace ayt::resource