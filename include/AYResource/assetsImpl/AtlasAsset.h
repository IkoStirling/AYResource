#pragma once

#include "AYResource/assetsDefs/IAtlas.h"

namespace ayt::resource
{

class AtlasAsset final : public IAtlas {
public:
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    const std::string& getTexturePath() const override { return _texturePath; }
    UInt32 getAtlasWidth() const override { return _atlasWidth; }
    UInt32 getAtlasHeight() const override { return _atlasHeight; }
    UInt32 getTileWidth() const override { return _tileWidth; }
    UInt32 getTileHeight() const override { return _tileHeight; }
    UInt32 getTilesPerRow() const override { return _tilesPerRow; }
    UInt32 getTilesPerColumn() const override { return _tilesPerColumn; }
    UInt32 getGutter() const override { return _gutter; }
    AtlasFilter getFilter() const override { return _filter; }
    AtlasWrap getWrapU() const override { return _wrapU; }
    AtlasWrap getWrapV() const override { return _wrapV; }

    bool loadFromBinary(const void* data, size_t size) override;
    bool saveToBinary(std::vector<UInt8>& outData) const override;

    bool create(std::string texturePath,
                UInt32 atlasWidth, UInt32 atlasHeight,
                UInt32 tileWidth, UInt32 tileHeight,
                UInt32 gutter, AtlasFilter filter,
                AtlasWrap wrapU, AtlasWrap wrapV);

private:
    void clear();

    std::string _texturePath;
    UInt32 _atlasWidth = 0;
    UInt32 _atlasHeight = 0;
    UInt32 _tileWidth = 0;
    UInt32 _tileHeight = 0;
    UInt32 _tilesPerRow = 0;
    UInt32 _tilesPerColumn = 0;
    UInt32 _gutter = 0;
    AtlasFilter _filter = AtlasFilter::Linear;
    AtlasWrap _wrapU = AtlasWrap::Clamp;
    AtlasWrap _wrapV = AtlasWrap::Clamp;
};

} // namespace ayt::resource
