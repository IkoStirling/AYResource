// AYTest_ResourcePipelineP4.cpp — P4: cookShipPackage → openDatabase → load from pak

#include "AYResource.h"
#include "AYResource/CookShip.h"
#include "AYResource/assetsImpl/Mesh.h"
#include "AYTest.h"

#include <AYIO/File.h>
#include <AYIO/Path.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ayt::resource;

namespace {

const std::string kAssets = "ayresource_p4_assets";
const std::string kShip = "ayresource_p4_ship";

bool writeBytes(const std::string& path, const std::vector<UInt8>& data)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

void resetManager()
{
    initializeLoaders();
    ResourceManager::instance().setOnHotReload({});
    ResourceManager::instance().setAutoWatchLoadedResources(false);
    ResourceManager::instance().unloadAll();
    ResourceManager::instance().setDatabase(ayt::storage::IStorageDatabase::create(":memory:"));
}

void cleanup()
{
    std::error_code ec;
    std::filesystem::remove_all(kAssets, ec);
    std::filesystem::remove_all(kShip, ec);
}

} // namespace

TEST_SUITE(ResourcePipelineP4Tests)

TEST_CASE(cook_ship_and_load_from_pak_via_db)
{
    cleanup();
    resetManager();

    const std::string meshDir = kAssets + "/meshes";
    std::filesystem::create_directories(meshDir);

    const std::string meshPath = meshDir + "/cube.aymesh";
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);
        std::vector<UInt8> bytes;
        CHECK(mesh->saveToBinary(bytes));
        CHECK(writeBytes(meshPath, bytes));
    }

    CookShipOptions opts;
    opts.assetsRoot = kAssets;
    opts.outputDir = kShip;
    opts.compression = ayt::storage::CompressionAlgo::None;

    const CookShipResult cooked = cookShipPackage(opts);
    CHECK(cooked.ok);
    CHECK(cooked.fileCount >= 1u);
    CHECK(ayt::io::File::exists(cooked.pakPath));
    CHECK(ayt::io::File::exists(cooked.dbPath));

    // Drop loose files so load must come from pak.
    std::filesystem::remove_all(kAssets);

    resetManager();
    CHECK(ResourceManager::instance().openDatabase(cooked.dbPath));

    const std::string logical = ayt::io::path::normalize("meshes/cube.aymesh");
    auto loaded = ResourceManager::instance().load<IMesh>(logical);
    CHECK(loaded != nullptr);
    CHECK(loaded->getVertexCount() > 0u);
    CHECK(ResourceManager::instance().getLoadState(logical) == ResourceLoadState::Ready);

    cleanup();
    resetManager();
}

TEST_SUITE_END
