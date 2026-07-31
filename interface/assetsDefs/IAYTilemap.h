#pragma once
#include "IAYResource.h"
#include <memory>

namespace ayt::resource
{

// ===== TilemapPackMode — tile-id storage width (mirrors ayt::ay2d::TileIdPackMode
// without taking an AY2D dependency; AYResource stays a generic asset layer and
// must not depend on a specific 2D runtime module. The bridge adapter maps
// this enum to ayt::ay2d::TileIdPackMode at the consumption boundary.)
enum class TilemapPackMode : UInt8 {
    Narrow16 = 0,  // uint16 per tile id (max 65535)
    Wide32   = 1,  // uint32 per tile id
};

// ===== ITilemap — 2D tilemap resource interface (L2 cache hand-out)
//
// design.md (AY2D §9.1 / §9.3): `.aytilemap` is an L1 disk file (magic 'AYTM',
// v1) decoded by AYResource into this L2 interface. Carries the tile-id array,
// grid dimensions, tile pixel size, pack mode, default tile id, and the set
// of "blocked" tile ids (the collision metadata the AYPhysics2DBridge consumes
// to build static bodies). No GPU handle lives here (L3 is AYRenderer's job).
//
// The blocked-id list is the source of truth for `ayt::ay2d::Tilemap::blockedTileIds`
// (AY2D §13.20): the loader reads it from disk, the bridge adapter copies it
// into the AY2D POD so `ITileCollisionQuery::isBlocked` returns Solid for those
// tile ids.
class ITilemap : public IResource {
public:
    virtual ~ITilemap() = default;

    // ===== Grid metadata =====
    virtual UInt32 getCols() const = 0;
    virtual UInt32 getRows() const = 0;
    virtual UInt16 getTileWidth() const = 0;
    virtual UInt16 getTileHeight() const = 0;
    virtual TilemapPackMode getPackMode() const = 0;
    virtual UInt32 getDefaultTileId() const = 0;

    // ===== Tile-id array (one of the two is non-null per pack mode) =====
    // Flat row-major [y*cols + x], count = cols*rows.
    virtual const UInt16* getTileIds16() const = 0;  // nullptr when Wide32
    virtual const UInt32* getTileIds32() const = 0;  // nullptr when Narrow16
    virtual UInt32 getTileIdCount() const = 0;

    // ===== Blocked (solid) tile-id list =====
    virtual const UInt32* getBlockedTileIds() const = 0;
    virtual UInt32 getBlockedTileIdCount() const = 0;

    // ===== Binary serialization =====
    virtual bool loadFromBinary(const void* data, size_t size) = 0;
    virtual bool saveToBinary(std::vector<UInt8>& outData) const = 0;

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x4D545941; // 'AYTM' little-endian (A=0x41,Y=0x59,T=0x54,M=0x4D)
};

} // namespace ayt::resource
