#pragma once

#include "AYResource/IResource.h"

#include <string>
#include <vector>

namespace ayt::resource
{

enum class AtlasFilter : UInt8 {
    Nearest = 0,
    Linear = 1,
    Tap4 = 2,
    Tap9 = 3,
};

enum class AtlasWrap : UInt8 {
    Clamp = 0,
    Repeat = 1,
    Mirror = 2,
};

class IAtlas : public IResource {
public:
    ~IAtlas() override = default;

    virtual const std::string& getTexturePath() const = 0;
    virtual UInt32 getAtlasWidth() const = 0;
    virtual UInt32 getAtlasHeight() const = 0;
    virtual UInt32 getTileWidth() const = 0;
    virtual UInt32 getTileHeight() const = 0;
    virtual UInt32 getTilesPerRow() const = 0;
    virtual UInt32 getTilesPerColumn() const = 0;
    virtual UInt32 getGutter() const = 0;
    virtual AtlasFilter getFilter() const = 0;
    virtual AtlasWrap getWrapU() const = 0;
    virtual AtlasWrap getWrapV() const = 0;

    virtual bool loadFromBinary(const void* data, size_t size) = 0;
    virtual bool saveToBinary(std::vector<UInt8>& outData) const = 0;

    static constexpr UInt32 VERSION = 1u;
    static constexpr UInt32 MAGIC = 0x54415941u; // 'AYAT' little-endian
};

} // namespace ayt::resource
