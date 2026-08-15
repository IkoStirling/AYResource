#include "AYResource.h"
#include "AYResource/assetsDefs/ITilemap.h"
#include "AYResource/assetsImpl/TilemapAsset.h"
#include "AYResource/Loader/TilemapLoader.h"
#include "AYResource/ResourceRegistry.h"
#include "AYResource/ResourceBootstrap.h"
#include "AYTest.h"
#include <cstring>

using namespace ayt::resource;

TEST_SUITE(TilemapLoaderTests)

TEST_CASE(CreateAndQueryNarrow16) {
    const TileCollisionFlagEntry flags[] = {
        {1u, TileCollisionFlagBits::Solid},
        {7u, TileCollisionFlagBits::Solid},
    };
    TilemapAsset t;
    t.create(4, 3, 32, 32, TilemapPackMode::Narrow16, 0u, flags, 2u);

    CHECK(t.getCols() == 4u);
    CHECK(t.getRows() == 3u);
    CHECK(t.getTileWidth() == 32u);
    CHECK(t.getTileHeight() == 32u);
    CHECK(t.getPackMode() == TilemapPackMode::Narrow16);
    CHECK(t.getDefaultTileId() == 0u);
    CHECK(t.getTileIdCount() == 12u);
    CHECK(t.getTileIds16() != nullptr);
    CHECK(t.getTileIds32() == nullptr);
    CHECK(t.getTileCollisionFlagCount() == 2u);
    CHECK(t.getTileCollisionFlags()[0].tileId == 1u);
    CHECK(t.getTileCollisionFlags()[0].flags == TileCollisionFlagBits::Solid);
    CHECK(t.getTileCollisionFlags()[1].tileId == 7u);
    CHECK(t.isLoaded() == true);
}

TEST_CASE(CreateAndQueryWide32) {
    const TileCollisionFlagEntry flags[] = {
        {65536u, TileCollisionFlagBits::Solid},  // > 65535, needs wide mode
    };
    TilemapAsset t;
    t.create(2, 2, 16, 16, TilemapPackMode::Wide32, 100u, flags, 1u);

    CHECK(t.getPackMode() == TilemapPackMode::Wide32);
    CHECK(t.getDefaultTileId() == 100u);
    CHECK(t.getTileIdCount() == 4u);
    CHECK(t.getTileIds32() != nullptr);
    CHECK(t.getTileIds16() == nullptr);
    CHECK(t.getTileIds32()[0] == 100u);
    CHECK(t.getTileCollisionFlagCount() == 1u);
    CHECK(t.getTileCollisionFlags()[0].tileId == 65536u);
    CHECK(t.getTileCollisionFlags()[0].flags == TileCollisionFlagBits::Solid);
}

TEST_CASE(SaveAndLoadBinaryRoundTripNarrow16) {
    const TileCollisionFlagEntry flags[] = {
        {1u, TileCollisionFlagBits::Solid},
        {2u, TileCollisionFlagBits::Solid},
        {9u, TileCollisionFlagBits::Solid},
    };
    TilemapAsset original;
    original.create(4, 4, 32, 32, TilemapPackMode::Narrow16, 0u, flags, 3u);
    // Paint a few cells so the tile-id array is non-trivial.
    UInt16* ids = const_cast<UInt16*>(original.getTileIds16());
    ids[0]  = 1u;   // (0,0) solid
    ids[15] = 9u;   // (3,3) solid

    std::vector<UInt8> binary;
    CHECK(original.saveToBinary(binary) == true);
    CHECK(binary.empty() == false);

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == true);

    CHECK(loaded.getCols() == 4u);
    CHECK(loaded.getRows() == 4u);
    CHECK(loaded.getTileWidth() == 32u);
    CHECK(loaded.getTileHeight() == 32u);
    CHECK(loaded.getPackMode() == TilemapPackMode::Narrow16);
    CHECK(loaded.getDefaultTileId() == 0u);
    CHECK(loaded.getTileIdCount() == 16u);
    CHECK(loaded.getTileIds16() != nullptr);
    CHECK(loaded.getTileIds16()[0] == 1u);
    CHECK(loaded.getTileIds16()[15] == 9u);
    CHECK(loaded.getTileCollisionFlagCount() == 3u);
    CHECK(loaded.getTileCollisionFlags()[0].tileId == 1u);
    CHECK(loaded.getTileCollisionFlags()[1].tileId == 2u);
    CHECK(loaded.getTileCollisionFlags()[2].tileId == 9u);
    CHECK(loaded.getTileCollisionFlags()[0].flags == TileCollisionFlagBits::Solid);
}

TEST_CASE(SaveAndLoadBinaryRoundTripWide32) {
    const TileCollisionFlagEntry flags[] = {{70000u, TileCollisionFlagBits::Solid}};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Wide32, 5u, flags, 1u);
    UInt32* ids = const_cast<UInt32*>(original.getTileIds32());
    ids[0] = 70000u;

    std::vector<UInt8> binary;
    CHECK(original.saveToBinary(binary) == true);

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == true);
    CHECK(loaded.getPackMode() == TilemapPackMode::Wide32);
    CHECK(loaded.getTileIdCount() == 4u);
    CHECK(loaded.getTileIds32() != nullptr);
    CHECK(loaded.getTileIds32()[0] == 70000u);
    CHECK(loaded.getDefaultTileId() == 5u);
    CHECK(loaded.getTileCollisionFlagCount() == 1u);
    CHECK(loaded.getTileCollisionFlags()[0].tileId == 70000u);
    CHECK(loaded.getTileCollisionFlags()[0].flags == TileCollisionFlagBits::Solid);
}

TEST_CASE(LoadFromBinaryRejectsBadMagic) {
    const TileCollisionFlagEntry flags[] = {{1u, TileCollisionFlagBits::Solid}};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, flags, 1u);
    std::vector<UInt8> binary;
    original.saveToBinary(binary);

    // Corrupt the magic.
    binary[0] = 0x00;

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == false);
    CHECK(loaded.isLoaded() == false);
}

TEST_CASE(LoadFromBinaryRejectsBadVersion) {
    const TileCollisionFlagEntry flags[] = {{1u, TileCollisionFlagBits::Solid}};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, flags, 1u);
    std::vector<UInt8> binary;
    original.saveToBinary(binary);

    // Corrupt the version (offset 4, UInt16) to an unsupported value.
    binary[4] = 0xFF;
    binary[5] = 0x0F;

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == false);
}

TEST_CASE(LoadFromBinaryRejectsTruncated) {
    const TileCollisionFlagEntry flags[] = {{1u, TileCollisionFlagBits::Solid}};
    TilemapAsset original;
    original.create(4, 4, 32, 32, TilemapPackMode::Narrow16, 0u, flags, 1u);
    std::vector<UInt8> binary;
    original.saveToBinary(binary);

    // Truncate to less than the 32-byte header so the header read fails.
    binary.resize(31);  // NOLINT: intentional under-size
    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == false);
}

TEST_CASE(CanLoad) {
    TilemapLoader loader;
    CHECK(loader.canLoad("level.aytilemap") == true);
    CHECK(loader.canLoad("path/to/level.aytilemap") == true);
    CHECK(loader.canLoad("level.aymesh") == false);
    CHECK(loader.canLoad("level.aytilemapx") == false);
}

TEST_CASE(GetResourceType) {
    TilemapLoader loader;
    CHECK(std::strcmp(loader.getResourceType(), "Tilemap") == 0);
}

TEST_CASE(LoaderLoadFromBinary) {
    const TileCollisionFlagEntry flags[] = {{1u, TileCollisionFlagBits::Solid}};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, flags, 1u);
    std::vector<UInt8> binary;
    original.saveToBinary(binary);

    TilemapLoader loader;
    auto resource = loader.loadFromBinary(binary.data(), binary.size());
    CHECK(resource != nullptr);

    auto tilemap = std::dynamic_pointer_cast<TilemapAsset>(resource);
    CHECK(tilemap != nullptr);
    CHECK(tilemap->getCols() == 2u);
    CHECK(tilemap->getRows() == 2u);
    CHECK(tilemap->getTileCollisionFlagCount() == 1u);
    CHECK(tilemap->getTileCollisionFlags()[0].flags == TileCollisionFlagBits::Solid);
}

TEST_CASE(SaveAndLoadBinaryRoundTripOneWayAndSlope) {
    // R3: non-Solid flags round-trip through the v2 binary format.
    const TileCollisionFlagEntry flags[] = {
        {1u, TileCollisionFlagBits::Solid},
        {2u, TileCollisionFlagBits::OneWay},
        {3u, TileCollisionFlagBits::Slope_L | TileCollisionFlagBits::Solid},
        {4u, TileCollisionFlagBits::Hazard},
    };
    TilemapAsset original;
    original.create(4, 1, 16, 16, TilemapPackMode::Narrow16, 0u, flags, 4u);

    std::vector<UInt8> binary;
    CHECK(original.saveToBinary(binary) == true);

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == true);
    CHECK(loaded.getTileCollisionFlagCount() == 4u);

    // Order is preserved (writer copies the table verbatim).
    const TileCollisionFlagEntry* f = loaded.getTileCollisionFlags();
    CHECK(f[0].tileId == 1u); CHECK(f[0].flags == TileCollisionFlagBits::Solid);
    CHECK(f[1].tileId == 2u); CHECK(f[1].flags == TileCollisionFlagBits::OneWay);
    CHECK(f[2].tileId == 3u); CHECK(f[2].flags == (TileCollisionFlagBits::Slope_L | TileCollisionFlagBits::Solid));
    CHECK(f[3].tileId == 4u); CHECK(f[3].flags == TileCollisionFlagBits::Hazard);
}

TEST_CASE(LoadFromBinaryV1BlockedListNormalizedToSolid) {
    // R3 back-compat: a hand-rolled v1 file (bare blocked-id list) loads and
    // normalizes every blocked id to a v2 entry with flags = Solid.
    #pragma pack(push, 1)
    struct V1Header {
        UInt32 magic; UInt16 version; UInt16 tileWidth; UInt16 tileHeight;
        UInt32 cols; UInt32 rows; UInt8 mode; UInt8 reserved;
        UInt32 defaultTileId; UInt32 blockedCount; UInt32 tileIdsCount;
    };
    #pragma pack(pop)
    static_assert(sizeof(V1Header) == 32, "v1 header must be 32 bytes");

    const UInt32 blockedIds[] = {1u, 9u};
    const UInt16 tileIds[] = {1u, 0u, 0u, 9u};  // 2x2

    V1Header h{};
    h.magic = ITilemap::MAGIC;
    h.version = 1u;
    h.tileWidth = 16; h.tileHeight = 16;
    h.cols = 2u; h.rows = 2u;
    h.mode = 0u;  // Narrow16
    h.defaultTileId = 0u;
    h.blockedCount = 2u;
    h.tileIdsCount = 4u;

    std::vector<UInt8> binary;
    binary.insert(binary.end(), reinterpret_cast<const UInt8*>(&h),
                   reinterpret_cast<const UInt8*>(&h) + sizeof(h));
    binary.insert(binary.end(), reinterpret_cast<const UInt8*>(blockedIds),
                   reinterpret_cast<const UInt8*>(blockedIds) + sizeof(blockedIds));
    binary.insert(binary.end(), reinterpret_cast<const UInt8*>(tileIds),
                   reinterpret_cast<const UInt8*>(tileIds) + sizeof(tileIds));

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == true);
    CHECK(loaded.getTileIdCount() == 4u);
    CHECK(loaded.getTileIds16()[0] == 1u);
    CHECK(loaded.getTileIds16()[3] == 9u);
    // v1 blocked ids normalized to Solid entries.
    CHECK(loaded.getTileCollisionFlagCount() == 2u);
    CHECK(loaded.getTileCollisionFlags()[0].tileId == 1u);
    CHECK(loaded.getTileCollisionFlags()[0].flags == TileCollisionFlagBits::Solid);
    CHECK(loaded.getTileCollisionFlags()[1].tileId == 9u);
    CHECK(loaded.getTileCollisionFlags()[1].flags == TileCollisionFlagBits::Solid);
}

TEST_CASE(BootstrapRegistersTilemapExtension) {
    CHECK(initializeLoaders());
    CHECK(ResourceRegistry::getTypeFromExtension(".aytilemap") == "Tilemap");

    auto loader = ResourceRegistry::createLoader("Tilemap");
    CHECK(loader != nullptr);
    CHECK(loader->getResourceType() == std::string("Tilemap"));
}

// ===== CM-5 (2026-08-12): animation table segment =====

static const TileAnimationEntry* findAnimEntry(const TilemapAsset& t,
                                               UInt32 sourceTileId) {
    const UInt32 count = t.getAnimationCount();
    const TileAnimationEntry* entries = t.getAnimationEntries();
    for (UInt32 i = 0u; i < count; ++i) {
        if (entries[i].sourceTileId == sourceTileId) {
            return &entries[i];
        }
    }
    return nullptr;
}

TEST_CASE(SetAnimationEntryReplaceAndRemove) {
    TilemapAsset t;
    t.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, nullptr, 0u);

    // null frames with frameCount > 0 is rejected.
    CHECK(t.setAnimationEntry(2u, nullptr, 1u) == false);
    CHECK(t.getAnimationCount() == 0u);
    CHECK(t.getAnimationEntries() == nullptr);

    const TileAnimationFrame water[] = {{10u, 100u}, {11u, 200u}};
    CHECK(t.setAnimationEntry(2u, water, 2u) == true);
    const TileAnimationFrame fire[] = {{20u, 50u}};
    CHECK(t.setAnimationEntry(7u, fire, 1u) == true);
    CHECK(t.getAnimationCount() == 2u);

    const TileAnimationEntry* e2 = findAnimEntry(t, 2u);
    CHECK(e2 != nullptr);
    CHECK(e2->frameCount == 2u);
    CHECK(e2->frames[0].frameTileId == 10u);
    CHECK(e2->frames[0].durationMs == 100u);
    CHECK(e2->frames[1].frameTileId == 11u);
    CHECK(e2->frames[1].durationMs == 200u);
    CHECK(findAnimEntry(t, 7u) != nullptr);

    // Replace semantics: same sourceTileId overwrites in place.
    const TileAnimationFrame lava[] = {{30u, 500u}};
    CHECK(t.setAnimationEntry(2u, lava, 1u) == true);
    CHECK(t.getAnimationCount() == 2u);
    e2 = findAnimEntry(t, 2u);
    CHECK(e2->frameCount == 1u);
    CHECK(e2->frames[0].frameTileId == 30u);
    CHECK(e2->frames[0].durationMs == 500u);

    // Removal via frameCount == 0; absent entry is a no-op success.
    CHECK(t.setAnimationEntry(7u, nullptr, 0u) == true);
    CHECK(findAnimEntry(t, 7u) == nullptr);
    CHECK(t.getAnimationCount() == 1u);
    CHECK(t.setAnimationEntry(99u, nullptr, 0u) == true);
    CHECK(t.getAnimationCount() == 1u);
}

TEST_CASE(SaveAndLoadBinaryRoundTripWithAnimations) {
    const TileCollisionFlagEntry flags[] = {{1u, TileCollisionFlagBits::Solid}};
    TilemapAsset original;
    original.create(4, 4, 32, 32, TilemapPackMode::Narrow16, 0u, flags, 1u);
    UInt16* ids = const_cast<UInt16*>(original.getTileIds16());
    ids[0]  = 1u;
    ids[15] = 9u;

    const TileAnimationFrame water[] = {{10u, 100u}, {11u, 150u}, {12u, 100u}};
    CHECK(original.setAnimationEntry(2u, water, 3u) == true);
    // Zero-duration frame is legal (the AY2D ticker breaks on it).
    const TileAnimationFrame lava[] = {{30u, 0u}, {31u, 250u}};
    CHECK(original.setAnimationEntry(9u, lava, 2u) == true);

    std::vector<UInt8> binary;
    CHECK(original.saveToBinary(binary) == true);

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == true);

    // Flags + tile ids unchanged by the segment round-trip.
    CHECK(loaded.getTileIdCount() == 16u);
    CHECK(loaded.getTileIds16()[0] == 1u);
    CHECK(loaded.getTileIds16()[15] == 9u);
    CHECK(loaded.getTileCollisionFlagCount() == 1u);
    CHECK(loaded.getTileCollisionFlags()[0].tileId == 1u);
    CHECK(loaded.getTileCollisionFlags()[0].flags == TileCollisionFlagBits::Solid);

    // Animation table round-trips frame-by-frame.
    CHECK(loaded.getAnimationCount() == 2u);
    const TileAnimationEntry* w = findAnimEntry(loaded, 2u);
    CHECK(w != nullptr);
    CHECK(w->frameCount == 3u);
    CHECK(w->frames[0].frameTileId == 10u);
    CHECK(w->frames[0].durationMs == 100u);
    CHECK(w->frames[1].frameTileId == 11u);
    CHECK(w->frames[1].durationMs == 150u);
    CHECK(w->frames[2].frameTileId == 12u);
    CHECK(w->frames[2].durationMs == 100u);
    const TileAnimationEntry* l = findAnimEntry(loaded, 9u);
    CHECK(l != nullptr);
    CHECK(l->frameCount == 2u);
    CHECK(l->frames[0].frameTileId == 30u);
    CHECK(l->frames[0].durationMs == 0u);
    CHECK(l->frames[1].frameTileId == 31u);
    CHECK(l->frames[1].durationMs == 250u);
}

TEST_CASE(LoadFromBinaryWithoutAnimationSegment) {
    // Files written before CM-5 end exactly after the tile ids. Reproduce by
    // stripping the always-written trailer (a zero count) from a fresh save.
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, nullptr, 0u);
    std::vector<UInt8> binary;
    original.saveToBinary(binary);
    binary.resize(binary.size() - sizeof(UInt32));  // strip animation count

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == true);
    CHECK(loaded.getTileIdCount() == 4u);
    CHECK(loaded.getTileIds16() != nullptr);
    CHECK(loaded.getAnimationCount() == 0u);
    CHECK(loaded.getAnimationEntries() == nullptr);
}

TEST_CASE(LoadFromBinaryRejectsTruncatedAnimationSegment) {
    const TileAnimationFrame water[] = {{10u, 100u}, {11u, 150u}, {12u, 100u}};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, nullptr, 0u);
    original.setAnimationEntry(2u, water, 3u);

    std::vector<UInt8> binary;
    original.saveToBinary(binary);
    binary.resize(binary.size() - sizeof(TileAnimationFrame));  // cut mid-frame

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == false);
    CHECK(loaded.isLoaded() == false);
}

TEST_CASE(LoadFromBinaryRejectsTrailingGarbage) {
    const TileAnimationFrame water[] = {{10u, 100u}, {11u, 150u}};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, nullptr, 0u);
    original.setAnimationEntry(2u, water, 2u);

    std::vector<UInt8> binary;
    original.saveToBinary(binary);
    binary.push_back(0xAB);  // one byte past a fully-valid segment

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == false);
}

TEST_CASE(LoadFromBinaryRejectsEmptyAnimationEntry) {
    // Hand-rolled: legacy file + segment whose entry has frameCount == 0
    // (the writer never produces one; the reader must reject it).
    TilemapAsset base;
    base.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, nullptr, 0u);
    std::vector<UInt8> binary;
    base.saveToBinary(binary);
    binary.resize(binary.size() - sizeof(UInt32));

    const UInt32 count = 1u, src = 5u, frameCount = 0u;
    const UInt8* p = reinterpret_cast<const UInt8*>(&count);
    binary.insert(binary.end(), p, p + sizeof(count));
    p = reinterpret_cast<const UInt8*>(&src);
    binary.insert(binary.end(), p, p + sizeof(src));
    p = reinterpret_cast<const UInt8*>(&frameCount);
    binary.insert(binary.end(), p, p + sizeof(frameCount));

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == false);
}

TEST_SUITE_END
