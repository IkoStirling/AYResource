#include "AYResource.h"
#include "AYResource/Converter/AtlasConverter.h"
#include "AYResource/Loader/AtlasLoader.h"
#include "AYResource/VirtualAssetPath.h"
#include "AYResource/assetsImpl/AtlasAsset.h"
#include "AYTest.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ayt::resource;

TEST_SUITE(AtlasResourceTests)

TEST_CASE(AtlasBinaryRoundTripPreservesSamplingContract)
{
    AtlasAsset original;
    CHECK_TRUE(original.create("textures/terrain.aytex", 256u, 128u,
                               32u, 32u, 1u, AtlasFilter::Tap4,
                               AtlasWrap::Repeat, AtlasWrap::Mirror));
    CHECK_INT_EQ(original.getTilesPerRow(), 8u);
    CHECK_INT_EQ(original.getTilesPerColumn(), 4u);

    std::vector<UInt8> bytes;
    CHECK_TRUE(original.saveToBinary(bytes));
    AtlasAsset loaded;
    CHECK_TRUE(loaded.loadFromBinary(bytes.data(), bytes.size()));
    CHECK_TRUE(loaded.getTexturePath() == "textures/terrain.aytex");
    CHECK_INT_EQ(loaded.getAtlasWidth(), 256u);
    CHECK_INT_EQ(loaded.getAtlasHeight(), 128u);
    CHECK_INT_EQ(loaded.getTileWidth(), 32u);
    CHECK_INT_EQ(loaded.getTileHeight(), 32u);
    CHECK_INT_EQ(loaded.getGutter(), 1u);
    CHECK(loaded.getFilter() == AtlasFilter::Tap4);
    CHECK(loaded.getWrapU() == AtlasWrap::Repeat);
    CHECK(loaded.getWrapV() == AtlasWrap::Mirror);
}

TEST_CASE(AtlasRejectsInvalidGeometryAndTrailingBytes)
{
    AtlasAsset invalid;
    CHECK_FALSE(invalid.create("textures/a.aytex", 255u, 128u,
                               32u, 32u, 1u, AtlasFilter::Linear,
                               AtlasWrap::Clamp, AtlasWrap::Clamp));

    AtlasAsset source;
    CHECK_TRUE(source.create("textures/a.aytex", 64u, 64u,
                             16u, 16u, 1u, AtlasFilter::Nearest,
                             AtlasWrap::Clamp, AtlasWrap::Clamp));
    std::vector<UInt8> bytes;
    CHECK_TRUE(source.saveToBinary(bytes));
    bytes.push_back(0xABu);
    CHECK_FALSE(invalid.loadFromBinary(bytes.data(), bytes.size()));
}

TEST_CASE(AtlasLoaderAndConverterClosedLoop)
{
    AtlasLoader loader;
    CHECK_TRUE(loader.canLoad("atlases/terrain.ayatlas"));
    CHECK_FALSE(loader.canLoad("atlases/terrain.ayatlas.json"));

    const char* sourcePath = "atlas_skeleton.ayatlas.json";
    const char* outputPath = "atlases/terrain_skeleton.ayatlas";
    {
        std::ofstream stream(sourcePath, std::ios::binary);
        stream << "{\"name\":\"terrain_skeleton\","
                  "\"texturePath\":\"textures/terrain.aytex\","
                  "\"atlasWidth\":256,\"atlasHeight\":128,"
                  "\"tileWidth\":32,\"tileHeight\":32,\"gutter\":1,"
                  "\"filter\":\"9tap\",\"wrapU\":\"clamp\","
                  "\"wrapV\":\"repeat\"}";
    }

    auto dispatched = IConverter::create(sourcePath);
    CHECK_NOT_NULL(dispatched.get());
    CHECK_TRUE(std::string(dispatched->getSourceType()) == "Atlas");
    AtlasConverter converter(sourcePath);
    converter.setOutputDir(".");
    const ConversionResult converted = converter.convert();
    CHECK_INT_EQ(static_cast<uint32_t>(converted.resources.size()), 1u);
    CHECK_TRUE(converted.resources[0].path
               == makeAtlasVirtualPath("terrain_skeleton"));

    auto resource = loader.load(outputPath);
    CHECK_NOT_NULL(resource.get());
    auto atlas = std::dynamic_pointer_cast<IAtlas>(resource);
    CHECK_NOT_NULL(atlas.get());
    CHECK(atlas->getFilter() == AtlasFilter::Tap9);
    CHECK(atlas->getWrapV() == AtlasWrap::Repeat);
    CHECK_INT_EQ(atlas->getTilesPerRow(), 8u);

    std::remove(sourcePath);
    std::remove(outputPath);
}

TEST_SUITE_END
