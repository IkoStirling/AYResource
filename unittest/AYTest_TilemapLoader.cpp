#include "AYResource.h"
#include "interface/assetsDefs/IAYTilemap.h"
#include "assetsImpl/AYTilemapAsset.h"
#include "Loader/TilemapLoader.h"
#include "AYResourceRegistry.h"
#include "AYResourceBootstrap.h"
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

TEST_SUITE_END
