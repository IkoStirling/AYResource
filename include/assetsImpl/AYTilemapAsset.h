#pragma once
#include "IAYTilemap.h"
#include <aymath/MathTypes.h>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== Tilemap — 2D tilemap resource implementation =====
//
// Decoded form of a `.aytilemap` L1 file (design.md AY2D §9.1). Holds the
// tile-id array (one of tileIds16/tileIds32 per pack mode), grid metadata,
// and the blocked (solid) tile-id list. Lives in the AYResource L2 cache,
// handed out via ITilemap. The AYPhysics2DBridge adapter copies this into
// the AY2D `ayt::ay2d::Tilemap` POD (which is what physics + rendering
// actually consume).
//
// Named `TilemapAsset` (not `Tilemap`) to avoid collision with the AY2D
// runtime `ayt::ay2d::Tilemap` POD at consumer call sites that open both
// namespaces — mirrors the `FontAsset` precedent in this module.
class TilemapAsset : public ITilemap {
public:
    TilemapAsset() = default;
    virtual ~TilemapAsset() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    // ===== ITilemap =====
    UInt32 getCols() const override            { return _cols; }
    UInt32 getRows() const override            { return _rows; }
    UInt16 getTileWidth() const override       { return _tileWidth; }
    UInt16 getTileHeight() const override      { return _tileHeight; }
    TilemapPackMode getPackMode() const override { return _mode; }
    UInt32 getDefaultTileId() const override    { return _defaultTileId; }

    const UInt16* getTileIds16() const override {
        return _mode == TilemapPackMode::Narrow16 ? _tileIds16.data() : nullptr;
    }
    const UInt32* getTileIds32() const override {
        return _mode == TilemapPackMode::Wide32 ? _tileIds32.data() : nullptr;
    }
    UInt32 getTileIdCount() const override {
        return static_cast<UInt32>(_mode == TilemapPackMode::Narrow16
                                       ? _tileIds16.size()
                                       : _tileIds32.size());
    }

    const TileCollisionFlagEntry* getTileCollisionFlags() const override {
        return _tileCollisionFlags.data();
    }
    UInt32 getTileCollisionFlagCount() const override {
        return static_cast<UInt32>(_tileCollisionFlags.size());
    }

    // ===== Binary serialization =====
    bool loadFromBinary(const void* data, size_t size) override;
    bool saveToBinary(std::vector<UInt8>& outData) const override;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== Create test data (mirrors Physics::createBox pattern) =====
    // Fills the grid with `defaultTileId`, sets the per-tile-id collision-flags
    // table, and marks the resource loaded. Used by loader round-trip tests and
    // the bridge end-to-end test to build a `.aytilemap` in memory without file
    // I/O. `flags` is an array of {tileId, bitmask} entries (TileCollisionFlagBits).
    void create(UInt32 cols, UInt32 rows,
                 UInt16 tileWidth, UInt16 tileHeight,
                 TilemapPackMode mode, UInt32 defaultTileId,
                 const TileCollisionFlagEntry* flags, UInt32 flagCount);

private:
    void clear();

    FGuid _guid{};

    UInt16 _tileWidth    = 0;
    UInt16 _tileHeight   = 0;
    UInt32 _cols         = 0;
    UInt32 _rows         = 0;
    TilemapPackMode _mode = TilemapPackMode::Narrow16;
    UInt32 _defaultTileId = 0;

    // Only one of these is populated at a time (driven by _mode).
    std::vector<UInt16> _tileIds16;
    std::vector<UInt32> _tileIds32;

    std::vector<TileCollisionFlagEntry> _tileCollisionFlags;

    std::string _name;
};

} // namespace ayt::resource
