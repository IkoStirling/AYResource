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

// ===== TileCollisionFlags — per-tile-id collision flag bits.
//
// R3 (design.md AY2D §8.1 / bridge §14.3): the `.aytilemap` carries a
// per-tile-id collision-flags table (tile id -> bitmask) instead of a
// bare "blocked id" list. AYResource stays a generic asset layer and does
// NOT depend on ayt::ay2d::CollisionFlags, so the bit layout is mirrored
// here as raw UInt32 constants. The bridge adapter casts these bits to
// ayt::ay2d::CollisionFlags at the consumption boundary (identical bit
// layout, so the cast is value-preserving).
namespace TileCollisionFlagBits {
    inline constexpr UInt32 None    = 0u;
    inline constexpr UInt32 Solid   = 1u << 0;
    inline constexpr UInt32 OneWay  = 1u << 1;
    inline constexpr UInt32 Slope_L = 1u << 2;
    inline constexpr UInt32 Slope_R = 1u << 3;
    inline constexpr UInt32 Hazard  = 1u << 4;
    inline constexpr UInt32 Ladder  = 1u << 5;
    inline constexpr UInt32 Empty   = 1u << 6;
}

// One entry of the per-tile-id collision-flags table. `flags` is a bitmask
// of TileCollisionFlagBits. A tile id absent from the table reads as Empty.
struct TileCollisionFlagEntry {
    UInt32 tileId;
    UInt32 flags;
};

// ===== Tile animation table (design.md AY2D §7.2) =====
// Per-source-tile-id flipbook: an animated tile id maps to a list of
// {frameTileId, durationMs} frames, cycled modulo by the consumer's tick
// (ayt::ay2d::tickTilemapAnimation on the AY2D side; the AYEntity
// TilemapAnimationRuntime mirror on the engine side). A tile id with no
// entry is static. Mirrors ayt::ay2d::TileFrame without taking an AY2D
// dependency (same rationale as TileCollisionFlagBits above); the bridge
// adapter copies these into the AY2D POD at the consumption boundary.
struct TileAnimationFrame {
    UInt32 frameTileId;   // tile id to display for this frame
    UInt32 durationMs;    // hold time in integer milliseconds (0 allowed)
};

// One entry of the animation table. `frames` points into resource-owned
// storage and is valid only while the resource is loaded (same contract as
// getTileIds16). `frameCount == 0` never occurs in a valid resource (the
// converter aborts and the binary loader rejects such entries).
struct TileAnimationEntry {
    UInt32 sourceTileId;
    UInt32 frameCount;
    const TileAnimationFrame* frames;
};

// ===== ITilemap — 2D tilemap resource interface (L2 cache hand-out)
//
// design.md (AY2D §9.1 / §9.3): `.aytilemap` is an L1 disk file (magic 'AYTM',
// v2) decoded by AYResource into this L2 interface. Carries the tile-id array,
// grid dimensions, tile pixel size, pack mode, default tile id, and the
// per-tile-id collision-flags table (the collision metadata the
// AYPhysics2DBridge consumes to build static bodies / one-way platforms /
// slopes). No GPU handle lives here (L3 is AYRenderer's job).
//
// The flags table is the source of truth for
// `ayt::ay2d::Tilemap::tileCollisionFlags` (AY2D §13.20 / R3): the loader
// reads it from disk, the bridge adapter copies it into the AY2D POD so
// `ITileCollisionQuery::flagsAt` returns the real per-tile flags.
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

    // ===== Per-tile-id collision-flags table (R3) =====
    // Flat array of {tileId, flags} entries; count = getTileCollisionFlagCount().
    // A tile id not present in the table reads as Empty. Order is unspecified;
    // consumers build a lookup map. v1 files (bare blocked-id list) are
    // normalized on load to entries with flags = TileCollisionFlagBits::Solid.
    virtual const TileCollisionFlagEntry* getTileCollisionFlags() const = 0;
    virtual UInt32 getTileCollisionFlagCount() const = 0;

    // ===== Per-source-tile-id animation table (design.md §7.2) =====
    // Flat array of non-empty entries; count = getAnimationCount(). The table
    // is sparse by sourceTileId — consumers derive maxSourceTileId by
    // scanning. A source tile id not present in the table is static. The v2
    // binary carries this table as an optional trailing segment (see
    // TilemapAsset::saveToBinary); files written before the segment existed
    // load with count == 0 (animation is an additive capability).
    virtual UInt32 getAnimationCount() const = 0;
    virtual const TileAnimationEntry* getAnimationEntries() const = 0;

    // ===== Binary serialization =====
    virtual bool loadFromBinary(const void* data, size_t size) = 0;
    virtual bool saveToBinary(std::vector<UInt8>& outData) const = 0;

    // ===== Constants =====
    // v1 = bare blocked-id list (back-compat read only; normalized to Solid).
    // v2 = per-tile-id collision-flags table (current write format).
    static constexpr UInt32 VERSION = 2;
    static constexpr UInt32 MAGIC = 0x4D545941; // 'AYTM' little-endian (A=0x41,Y=0x59,T=0x54,M=0x4D)
};

} // namespace ayt::resource
