// AYTest_TilemapConverter.cpp — CM-2 (2026-08-11) acceptance cases.
//
// Covers the .aytilemap.json -> .aytilemap v2 Converter:
//   1. round-trip: author JSON -> convert() -> on-disk binary ->
//      TilemapAsset::loadFromBinary — every field asserted.
//   2. Guid determinism: same input twice -> same guid; one changed
//      tile -> different guid (cook-cache correctness).
//   3. defaults: absent "tiles" -> all defaultTileId; absent
//      "collisionFlags" -> empty table.
//   4. invalid inputs: missing source / bad JSON -> empty result,
//      no throw.
//   5. dispatch + path contract: IConverter::create(".aytilemap.json")
//      hits TilemapConverter (before the generic extension check) and
//      makeTilemapVirtualPath spelling is exact.

#include "AYResource.h"
#include "assetsImpl/AYTilemapAsset.h"
#include "Converter/TilemapConverter.h"
#include "AYVirtualAssetPath.h"
#include "AYTest.h"

#include <fstream>
#include <string>

using namespace ayt::resource;

namespace {

bool writeTextFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return false;
    }
    f << content;
    return f.good();
}

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

std::string readBinaryFile(const std::string& path, std::vector<UInt8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return "";
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
    }
    return path;
}

// 16x12 grid, 6 explicit tiles, solid+oneWay on tile 5 — the fixture
// used by the round-trip and guid cases.
const char* kAuthorJson =
    "{\n"
    "  \"name\": \"ground\",\n"
    "  \"cols\": 16, \"rows\": 12,\n"
    "  \"tileWidth\": 32, \"tileHeight\": 32,\n"
    "  \"defaultTileId\": 5,\n"
    "  \"mode\": \"narrow16\",\n"
    "  \"tiles\": [1, 2, 3, 4, 6, 7],\n"
    "  \"collisionFlags\": [\n"
    "    {\"tileId\": 5, \"flags\": \"solid|oneWay\"},\n"
    "    {\"tileId\": 9, \"flags\": \"0x4\"}\n"
    "  ]\n"
    "}\n";

const char* kOutputDir = ".";

} // namespace

TEST_SUITE(TilemapConverterTests)

// ─── #1 — round-trip through disk + loadFromBinary. ────────────────
TEST_CASE(TilemapJsonToBinaryRoundTrip)
{
    CHECK(writeTextFile("cm2_ground.aytilemap.json", kAuthorJson));

    TilemapConverter converter("cm2_ground.aytilemap.json");
    converter.setOutputDir(kOutputDir);
    CHECK_TRUE(converter.isValid());
    ConversionResult result = converter.convert();
    CHECK_INT_EQ(static_cast<int>(result.resources.size()), 1);

    const ConversionResult::ConvertedResource& res = result.resources[0];
    CHECK_TRUE(res.path == "tilemaps/ground.aytilemap");
    CHECK_TRUE(std::string(res.type) == "Tilemap");
    CHECK(res.size > 0u);
    CHECK_FALSE(res.guid.isZero());
    CHECK_TRUE(fileExists("tilemaps/ground.aytilemap"));

    // Load the cooked binary back and assert every field.
    std::vector<UInt8> binary;
    readBinaryFile("tilemaps/ground.aytilemap", binary);
    CHECK_FALSE(binary.empty());

    TilemapAsset loaded;
    CHECK_TRUE(loaded.loadFromBinary(binary.data(), binary.size()));
    CHECK_INT_EQ(loaded.getCols(), 16u);
    CHECK_INT_EQ(loaded.getRows(), 12u);
    CHECK_INT_EQ(loaded.getTileWidth(), 32u);
    CHECK_INT_EQ(loaded.getTileHeight(), 32u);
    CHECK(loaded.getPackMode() == TilemapPackMode::Narrow16);
    CHECK_INT_EQ(loaded.getDefaultTileId(), 5u);
    CHECK_INT_EQ(loaded.getTileIdCount(), 16u * 12u);
    CHECK_NOT_NULL(loaded.getTileIds16());
    CHECK_INT_EQ(loaded.getTileIds16()[0], 1u);
    CHECK_INT_EQ(loaded.getTileIds16()[1], 2u);
    CHECK_INT_EQ(loaded.getTileIds16()[5], 7u);
    CHECK_INT_EQ(loaded.getTileIds16()[6], 5u);   // beyond tiles list = default
    CHECK_INT_EQ(loaded.getTileIds16()[191], 5u);

    CHECK_INT_EQ(loaded.getTileCollisionFlagCount(), 2u);
    CHECK_INT_EQ(loaded.getTileCollisionFlags()[0].tileId, 5u);
    CHECK(loaded.getTileCollisionFlags()[0].flags
          == (TileCollisionFlagBits::Solid | TileCollisionFlagBits::OneWay));
    CHECK_INT_EQ(loaded.getTileCollisionFlags()[1].tileId, 9u);
    CHECK_INT_EQ(loaded.getTileCollisionFlags()[1].flags,
                 TileCollisionFlagBits::Slope_L);

    std::remove("cm2_ground.aytilemap.json");
    std::remove("tilemaps/ground.aytilemap");
}

// ─── #2 — guid determinism + sensitivity to a single tile change. ──
TEST_CASE(TilemapConverterGuidDeterministic)
{
    CHECK(writeTextFile("cm2_guid_a.aytilemap.json", kAuthorJson));

    TilemapConverter a("cm2_guid_a.aytilemap.json");
    const ConversionResult ra = a.convert();
    const ConversionResult rb = a.convert();  // same source, twice
    CHECK_INT_EQ(static_cast<int>(ra.resources.size()), 1);
    CHECK_INT_EQ(static_cast<int>(rb.resources.size()), 1);
    CHECK(ra.resources[0].guid == rb.resources[0].guid);

    // One tile value changed => guid must differ (cook-cache correctness).
    std::string changed = kAuthorJson;
    const size_t pos = changed.find("[1, 2, 3, 4, 6, 7]");
    CHECK(pos != std::string::npos);
    changed.replace(pos, std::string("[1, 2, 3, 4, 6, 7]").size(), "[1, 2, 9, 4, 6, 7]");
    CHECK(writeTextFile("cm2_guid_b.aytilemap.json", changed));

    TilemapConverter b("cm2_guid_b.aytilemap.json");
    const ConversionResult rc = b.convert();
    CHECK_INT_EQ(static_cast<int>(rc.resources.size()), 1);
    CHECK_FALSE(rc.resources[0].guid == ra.resources[0].guid);

    std::remove("cm2_guid_a.aytilemap.json");
    std::remove("cm2_guid_b.aytilemap.json");
}

// ─── #3 — defaults: no tiles / no collisionFlags. ──────────────────
TEST_CASE(TilemapConverterDefaults)
{
    const char* minimal =
        "{\"name\": \"flat\", \"cols\": 4, \"rows\": 3,"
        " \"tileWidth\": 16, \"tileHeight\": 16,"
        " \"defaultTileId\": 7}\n";
    CHECK(writeTextFile("cm2_flat.aytilemap.json", minimal));

    TilemapConverter converter("cm2_flat.aytilemap.json");
    converter.setOutputDir(kOutputDir);
    const ConversionResult result = converter.convert();
    CHECK_INT_EQ(static_cast<int>(result.resources.size()), 1);

    std::vector<UInt8> binary;
    readBinaryFile("tilemaps/flat.aytilemap", binary);
    CHECK_FALSE(binary.empty());

    TilemapAsset loaded;
    CHECK_TRUE(loaded.loadFromBinary(binary.data(), binary.size()));
    CHECK_INT_EQ(loaded.getCols(), 4u);
    CHECK_INT_EQ(loaded.getRows(), 3u);
    CHECK_INT_EQ(loaded.getTileIdCount(), 12u);
    CHECK_INT_EQ(loaded.getTileCollisionFlagCount(), 0u);
    for (UInt32 i = 0; i < 12u; ++i) {
        CHECK_INT_EQ(loaded.getTileIds16()[i], 7u);
    }

    std::remove("cm2_flat.aytilemap.json");
    std::remove("tilemaps/flat.aytilemap");
}

// ─── #4 — invalid inputs: missing source / bad JSON, no throw. ─────
TEST_CASE(TilemapConverterInvalidInputs)
{
    // Source file does not exist.
    TilemapConverter missing("cm2_does_not_exist.aytilemap.json");
    const ConversionResult r1 = missing.convert();
    CHECK_INT_EQ(static_cast<int>(r1.resources.size()), 0);

    // Source exists but is not valid JSON.
    CHECK(writeTextFile("cm2_bad.aytilemap.json", "this is not json {"));
    TilemapConverter bad("cm2_bad.aytilemap.json");
    const ConversionResult r2 = bad.convert();
    CHECK_INT_EQ(static_cast<int>(r2.resources.size()), 0);

    // Invalid geometry must also fail cleanly (no garbage binary).
    CHECK(writeTextFile("cm2_zero.aytilemap.json",
                        "{\"name\": \"z\", \"cols\": 0, \"rows\": 0,"
                        " \"tileWidth\": 0, \"tileHeight\": 0}"));
    TilemapConverter zero("cm2_zero.aytilemap.json");
    const ConversionResult r3 = zero.convert();
    CHECK_INT_EQ(static_cast<int>(r3.resources.size()), 0);

    std::remove("cm2_bad.aytilemap.json");
    std::remove("cm2_zero.aytilemap.json");
}

// ─── #5 — dispatch + path spelling contract. ───────────────────────
TEST_CASE(TilemapConverterDispatchAndPath)
{
    // create() must route .aytilemap.json BEFORE the generic extension
    // check (last extension is "json").
    auto converter = IConverter::create("map.aytilemap.json");
    CHECK_NOT_NULL(converter.get());
    CHECK_TRUE(std::string(converter->getSourceType()) == "Tilemap");

    // Plain .json still has no converter.
    CHECK_NULL(IConverter::create("map.json").get());

    // Exact virtual-path spelling (single source of truth).
    CHECK_TRUE(makeTilemapVirtualPath("ground") == "tilemaps/ground.aytilemap");
    CHECK_TRUE(makeTilemapVirtualPath("hero_sheet") == "tilemaps/hero_sheet.aytilemap");
}

TEST_SUITE_END
