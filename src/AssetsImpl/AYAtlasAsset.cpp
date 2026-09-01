#include "AYResource/assetsImpl/AtlasAsset.h"

#include <AYIO/File.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace ayt::resource
{

#pragma pack(push, 1)
struct AtlasBinaryHeader {
    UInt32 magic;
    UInt16 version;
    UInt16 reserved0;
    UInt32 atlasWidth;
    UInt32 atlasHeight;
    UInt32 tileWidth;
    UInt32 tileHeight;
    UInt32 tilesPerRow;
    UInt32 tilesPerColumn;
    UInt32 gutter;
    UInt8 filter;
    UInt8 wrapU;
    UInt8 wrapV;
    UInt8 reserved1;
    UInt32 texturePathBytes;
};
#pragma pack(pop)
static_assert(sizeof(AtlasBinaryHeader) == 44u, "AYAT v1 header size");

void AtlasAsset::clear()
{
    _texturePath.clear();
    _atlasWidth = _atlasHeight = _tileWidth = _tileHeight = 0u;
    _tilesPerRow = _tilesPerColumn = _gutter = 0u;
    _filter = AtlasFilter::Linear;
    _wrapU = _wrapV = AtlasWrap::Clamp;
    _loaded = false;
}

bool AtlasAsset::unload()
{
    clear();
    return true;
}

size_t AtlasAsset::sizeInBytes() const
{
    return sizeof(AtlasAsset) + _texturePath.size();
}

bool AtlasAsset::create(std::string texturePath,
                        UInt32 atlasWidth, UInt32 atlasHeight,
                        UInt32 tileWidth, UInt32 tileHeight,
                        UInt32 gutter, AtlasFilter filter,
                        AtlasWrap wrapU, AtlasWrap wrapV)
{
    clear();
    if (texturePath.empty() || atlasWidth == 0u || atlasHeight == 0u
        || tileWidth == 0u || tileHeight == 0u
        || atlasWidth % tileWidth != 0u || atlasHeight % tileHeight != 0u
        || gutter >= (std::min(tileWidth, tileHeight) / 2u)
        || static_cast<UInt8>(filter) > static_cast<UInt8>(AtlasFilter::Tap9)
        || static_cast<UInt8>(wrapU) > static_cast<UInt8>(AtlasWrap::Mirror)
        || static_cast<UInt8>(wrapV) > static_cast<UInt8>(AtlasWrap::Mirror)) {
        return false;
    }
    _texturePath = std::move(texturePath);
    _atlasWidth = atlasWidth;
    _atlasHeight = atlasHeight;
    _tileWidth = tileWidth;
    _tileHeight = tileHeight;
    _tilesPerRow = atlasWidth / tileWidth;
    _tilesPerColumn = atlasHeight / tileHeight;
    _gutter = gutter;
    _filter = filter;
    _wrapU = wrapU;
    _wrapV = wrapV;
    _type = "Atlas";
    _loaded = true;
    return true;
}

bool AtlasAsset::load(const std::string& path)
{
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) return false;
    std::vector<UInt8> bytes(file.size());
    if (!bytes.empty() && file.read(bytes.data(), bytes.size()) != bytes.size()) {
        return false;
    }
    if (!loadFromBinary(bytes.data(), bytes.size())) return false;
    _path = path;
    return true;
}

bool AtlasAsset::loadFromBinary(const void* data, size_t size)
{
    if (data == nullptr || size < sizeof(AtlasBinaryHeader)) return false;
    AtlasBinaryHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != IAtlas::MAGIC || header.version != IAtlas::VERSION
        || header.filter > static_cast<UInt8>(AtlasFilter::Tap9)
        || header.wrapU > static_cast<UInt8>(AtlasWrap::Mirror)
        || header.wrapV > static_cast<UInt8>(AtlasWrap::Mirror)
        || static_cast<uint64_t>(sizeof(header)) + header.texturePathBytes != size) {
        return false;
    }
    const char* path = static_cast<const char*>(data) + sizeof(header);
    std::string texturePath(path, path + header.texturePathBytes);
    if (!create(std::move(texturePath), header.atlasWidth, header.atlasHeight,
                header.tileWidth, header.tileHeight, header.gutter,
                static_cast<AtlasFilter>(header.filter),
                static_cast<AtlasWrap>(header.wrapU),
                static_cast<AtlasWrap>(header.wrapV))) {
        return false;
    }
    return _tilesPerRow == header.tilesPerRow
        && _tilesPerColumn == header.tilesPerColumn;
}

bool AtlasAsset::saveToBinary(std::vector<UInt8>& outData) const
{
    if (!_loaded || _texturePath.empty()
        || _texturePath.size() > std::numeric_limits<UInt32>::max()) return false;
    AtlasBinaryHeader header{};
    header.magic = IAtlas::MAGIC;
    header.version = static_cast<UInt16>(IAtlas::VERSION);
    header.atlasWidth = _atlasWidth;
    header.atlasHeight = _atlasHeight;
    header.tileWidth = _tileWidth;
    header.tileHeight = _tileHeight;
    header.tilesPerRow = _tilesPerRow;
    header.tilesPerColumn = _tilesPerColumn;
    header.gutter = _gutter;
    header.filter = static_cast<UInt8>(_filter);
    header.wrapU = static_cast<UInt8>(_wrapU);
    header.wrapV = static_cast<UInt8>(_wrapV);
    header.texturePathBytes = static_cast<UInt32>(_texturePath.size());
    outData.resize(sizeof(header) + _texturePath.size());
    std::memcpy(outData.data(), &header, sizeof(header));
    std::memcpy(outData.data() + sizeof(header), _texturePath.data(),
                _texturePath.size());
    return true;
}

} // namespace ayt::resource
