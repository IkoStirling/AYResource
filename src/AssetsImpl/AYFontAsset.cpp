#include "AYFontAsset.h"
#include <AYMath/MathTypes.h>
#include <AYIO/File.h>
#include <cstring>

namespace ayt::resource
{

// ===== FontAsset 二进制格式 =====
#pragma pack(push, 1)
struct FontBinaryHeader {
    UInt32 magic;              // 'AYFN' = 0x4E465941
    UInt32 version;            // 版本 = 1
    FGuid guid;                // 资源唯一标识 (16 bytes)
    UInt32 nameLength;         // 名称长度
    // name[nameLength] 在这里
    UInt32 fontSize;           // 字体大小
    UInt32 atlasWidth;         // Atlas 宽度
    UInt32 atlasHeight;        // Atlas 高度
    UInt32 glyphCount;         // 字形数量
    // GlyphTable[glyphCount] 在这里
    // AtlasData 在最后
};

struct FontGlyphData {
    UInt32 codepoint;          // Unicode 码点
    float u, v;                // UV 起始
    float w, h;                // 宽高
    float advance;             // 前进量
};
#pragma pack(pop)

// ===== 常量 =====
// ===== 常量 =====

// ===== FontAsset =====

FontAsset::FontAsset() = default;

void FontAsset::clear() {
    _name.clear();
    _fontSize = 0;
    _atlasWidth = 0;
    _atlasHeight = 0;
    _glyphs.clear();
    _glyphMap.clear();
    _atlasData.clear();
}

bool FontAsset::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t FontAsset::sizeInBytes() const {
    return sizeof(FontAsset) + _atlasData.size() + (_glyphs.size() * sizeof(Glyph));
}

bool FontAsset::load(const std::string& path) {
    _path = path;

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(FontBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool FontAsset::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(FontBinaryHeader)) {
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const FontBinaryHeader* header = reinterpret_cast<const FontBinaryHeader*>(ptr);

    // 验证 magic 和 version
    if (header->magic != IFontAsset::MAGIC || header->version != IFontAsset::VERSION) {
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取基本属性
    _fontSize = header->fontSize;
    _atlasWidth = header->atlasWidth;
    _atlasHeight = header->atlasHeight;

    // 读取 name
    size_t offset = sizeof(FontBinaryHeader);
    if (header->nameLength > 0) {
        if (size < offset + header->nameLength) {
            return false;
        }
        _name.assign(reinterpret_cast<const char*>(ptr + offset), header->nameLength);
        offset += header->nameLength;
    } else {
        _name.clear();
    }

    // 读取 glyphs
    _glyphs.resize(header->glyphCount);
    for (UInt32 i = 0; i < header->glyphCount; i++) {
        if (offset + sizeof(FontGlyphData) > size) {
            return false;
        }
        const FontGlyphData* glyphData = reinterpret_cast<const FontGlyphData*>(ptr + offset);
        _glyphs[i].codepoint = glyphData->codepoint;
        _glyphs[i].u = glyphData->u;
        _glyphs[i].v = glyphData->v;
        _glyphs[i].w = glyphData->w;
        _glyphs[i].h = glyphData->h;
        _glyphs[i].advance = glyphData->advance;
        offset += sizeof(FontGlyphData);
    }

    // 读取 atlasData
    if (offset < size) {
        size_t atlasSize = size - offset;
        _atlasData.resize(atlasSize);
        std::memcpy(_atlasData.data(), ptr + offset, atlasSize);
    }

    // 重建 _glyphMap
    _glyphMap.clear();
    for (UInt32 i = 0; i < static_cast<UInt32>(_glyphs.size()); i++) {
        _glyphMap[_glyphs[i].codepoint] = i;
    }

    _loaded = true;
    return true;
}

bool FontAsset::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t nameLen = _name.size();
    size_t glyphDataSize = _glyphs.size() * sizeof(FontGlyphData);
    size_t totalSize = sizeof(FontBinaryHeader) + nameLen + glyphDataSize + _atlasData.size();

    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Custom Header
    FontBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = IFontAsset::MAGIC;
    header.version = IFontAsset::VERSION;
    header.guid = _guid;
    header.nameLength = static_cast<UInt32>(nameLen);
    header.fontSize = _fontSize;
    header.atlasWidth = _atlasWidth;
    header.atlasHeight = _atlasHeight;
    header.glyphCount = static_cast<UInt32>(_glyphs.size());

    std::memcpy(ptr, &header, sizeof(header));

    // 写入 name
    size_t offset = sizeof(FontBinaryHeader);
    if (nameLen > 0) {
        std::memcpy(ptr + offset, _name.data(), nameLen);
        offset += nameLen;
    }

    // 写入 glyphs
    for (size_t i = 0; i < _glyphs.size(); i++) {
        FontGlyphData glyphData;
        glyphData.codepoint = _glyphs[i].codepoint;
        glyphData.u = _glyphs[i].u;
        glyphData.v = _glyphs[i].v;
        glyphData.w = _glyphs[i].w;
        glyphData.h = _glyphs[i].h;
        glyphData.advance = _glyphs[i].advance;
        std::memcpy(ptr + offset, &glyphData, sizeof(glyphData));
        offset += sizeof(glyphData);
    }

    // 写入 atlasData
    if (!_atlasData.empty()) {
        std::memcpy(ptr + offset, _atlasData.data(), _atlasData.size());
    }

    return true;
}

void FontAsset::addGlyph(const Glyph& glyph) {
    UInt32 index = static_cast<UInt32>(_glyphs.size());
    _glyphs.push_back(glyph);
    _glyphMap[glyph.codepoint] = index;
}

void FontAsset::setAtlasData(const UInt8* data, size_t size) {
    _atlasData.resize(size);
    memcpy(_atlasData.data(), data, size);
}

} // namespace ayt::resource