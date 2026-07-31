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
    const UInt32 blocked[] = {1u, 7u};
    TilemapAsset t;
    t.create(4, 3, 32, 32, TilemapPackMode::Narrow16, 0u, blocked, 2u);

    CHECK(t.getCols() == 4u);
    CHECK(t.getRows() == 3u);
    CHECK(t.getTileWidth() == 32u);
    CHECK(t.getTileHeight() == 32u);
    CHECK(t.getPackMode() == TilemapPackMode::Narrow16);
    CHECK(t.getDefaultTileId() == 0u);
    CHECK(t.getTileIdCount() == 12u);
    CHECK(t.getTileIds16() != nullptr);
    CHECK(t.getTileIds32() == nullptr);
    CHECK(t.getBlockedTileIdCount() == 2u);
    CHECK(t.getBlockedTileIds()[0] == 1u);
    CHECK(t.getBlockedTileIds()[1] == 7u);
    CHECK(t.isLoaded() == true);
}

TEST_CASE(CreateAndQueryWide32) {
    const UInt32 blocked[] = {65536u};  // > 65535, needs wide mode
    TilemapAsset t;
    t.create(2, 2, 16, 16, TilemapPackMode::Wide32, 100u, blocked, 1u);

    CHECK(t.getPackMode() == TilemapPackMode::Wide32);
    CHECK(t.getDefaultTileId() == 100u);
    CHECK(t.getTileIdCount() == 4u);
    CHECK(t.getTileIds32() != nullptr);
    CHECK(t.getTileIds16() == nullptr);
    CHECK(t.getTileIds32()[0] == 100u);
    CHECK(t.getBlockedTileIdCount() == 1u);
    CHECK(t.getBlockedTileIds()[0] == 65536u);
}

TEST_CASE(SaveAndLoadBinaryRoundTripNarrow16) {
    const UInt32 blocked[] = {1u, 2u, 9u};
    TilemapAsset original;
    original.create(4, 4, 32, 32, TilemapPackMode::Narrow16, 0u, blocked, 3u);
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
    CHECK(loaded.getBlockedTileIdCount() == 3u);
    CHECK(loaded.getBlockedTileIds()[0] == 1u);
    CHECK(loaded.getBlockedTileIds()[1] == 2u);
    CHECK(loaded.getBlockedTileIds()[2] == 9u);
}

TEST_CASE(SaveAndLoadBinaryRoundTripWide32) {
    const UInt32 blocked[] = {70000u};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Wide32, 5u, blocked, 1u);
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
    CHECK(loaded.getBlockedTileIdCount() == 1u);
    CHECK(loaded.getBlockedTileIds()[0] == 70000u);
}

TEST_CASE(LoadFromBinaryRejectsBadMagic) {
    const UInt32 blocked[] = {1u};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, blocked, 1u);
    std::vector<UInt8> binary;
    original.saveToBinary(binary);

    // Corrupt the magic.
    binary[0] = 0x00;

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == false);
    CHECK(loaded.isLoaded() == false);
}

TEST_CASE(LoadFromBinaryRejectsBadVersion) {
    const UInt32 blocked[] = {1u};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, blocked, 1u);
    std::vector<UInt8> binary;
    original.saveToBinary(binary);

    // Corrupt the version (offset 4, UInt16).
    binary[4] = 0xFF;
    binary[5] = 0x0F;

    TilemapAsset loaded;
    CHECK(loaded.loadFromBinary(binary.data(), binary.size()) == false);
}

TEST_CASE(LoadFromBinaryRejectsTruncated) {
    const UInt32 blocked[] = {1u};
    TilemapAsset original;
    original.create(4, 4, 32, 32, TilemapPackMode::Narrow16, 0u, blocked, 1u);
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
    const UInt32 blocked[] = {1u};
    TilemapAsset original;
    original.create(2, 2, 16, 16, TilemapPackMode::Narrow16, 0u, blocked, 1u);
    std::vector<UInt8> binary;
    original.saveToBinary(binary);

    TilemapLoader loader;
    auto resource = loader.loadFromBinary(binary.data(), binary.size());
    CHECK(resource != nullptr);

    auto tilemap = std::dynamic_pointer_cast<TilemapAsset>(resource);
    CHECK(tilemap != nullptr);
    CHECK(tilemap->getCols() == 2u);
    CHECK(tilemap->getRows() == 2u);
    CHECK(tilemap->getBlockedTileIdCount() == 1u);
}

TEST_CASE(BootstrapRegistersTilemapExtension) {
    CHECK(initializeLoaders());
    CHECK(ResourceRegistry::getTypeFromExtension(".aytilemap") == "Tilemap");

    auto loader = ResourceRegistry::createLoader("Tilemap");
    CHECK(loader != nullptr);
    CHECK(loader->getResourceType() == std::string("Tilemap"));
}

TEST_SUITE_END
