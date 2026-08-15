// AYTest_ResourcePipelineP1.cpp — P1: intrinsic L2 deps + LoadState / placeholders
//
// Covers:
//   - mesh material slots preload .aymat without .aydep.json
//   - material texture params preload .aytex
//   - missing .aytex → Failed + magenta placeholder still cached
//   - createHandle goes through ResourceManager::_loadInternal (deps apply)

#include "AYResource.h"
#include "AYLooseDependency.h"
#include "AYMesh.h"
#include "AYMaterial.h"
#include "AYTexture.h"
#include "AYTest.h"
#include "AYStorage/IStorageDatabase.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ayt::resource;

namespace {

const std::string kDir = "ayresource_p1_tmp";

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
    ResourceManager::instance().setDatabase(ayt::storage::IStorageDatabase::create(":memory:"));
    ResourceManager::instance().unloadAll();
}

void cleanupDir()
{
    std::error_code ec;
    std::filesystem::remove_all(kDir, ec);
}

} // namespace

TEST_SUITE(ResourcePipelineP1Tests)

TEST_CASE(intrinsic_mesh_slot_preloads_material_without_sidecar)
{
    cleanupDir();
    std::filesystem::create_directories(kDir);
    resetManager();

    const std::string matPath = kDir + "/hero.aymat";
    const std::string meshPath = kDir + "/hero.aymesh";

    {
        auto mat = std::make_shared<Material>();
        mat->createDefault();
        std::vector<UInt8> bytes;
        CHECK(mat->saveToBinary(bytes));
        CHECK(writeBytes(matPath, bytes));
    }
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);
        mesh->_addForTestMaterialSlot("hero.aymat");
        std::vector<UInt8> bytes;
        CHECK(mesh->saveToBinary(bytes));
        CHECK(writeBytes(meshPath, bytes));
    }

    // No .aydep.json next to the mesh — intrinsic slots must drive the graph.
    CHECK(!std::filesystem::exists(kDir + "/hero.aydep.json"));

    auto loaded = ResourceManager::instance().load<IMesh>(meshPath);
    CHECK(loaded != nullptr);
    CHECK(ResourceManager::instance().getLoadState(meshPath) == ResourceLoadState::Ready);
    CHECK(ResourceManager::instance().isLoaded(matPath));
    CHECK(ResourceManager::instance().getLoadState(matPath) == ResourceLoadState::Ready);

    // createCube seeds "default.aymat"; _addForTestMaterialSlot appends hero.aymat.
    const auto intrinsic = collectIntrinsicDependencies(meshPath, *loaded);
    CHECK(intrinsic.size() >= 1u);
    bool foundHero = false;
    for (const std::string& dep : intrinsic) {
        if (dep.find("hero.aymat") != std::string::npos) {
            foundHero = true;
            break;
        }
    }
    CHECK(foundHero);

    cleanupDir();
}

TEST_CASE(intrinsic_material_texture_missing_installs_placeholder)
{
    cleanupDir();
    std::filesystem::create_directories(kDir);
    resetManager();

    const std::string matPath = kDir + "/with_tex.aymat";
    const std::string texPath = kDir + "/missing.aytex";

    {
        auto mat = std::make_shared<Material>();
        mat->createDefault();
        mat->setTexture("albedoMap", "missing.aytex");
        std::vector<UInt8> bytes;
        CHECK(mat->saveToBinary(bytes));
        CHECK(writeBytes(matPath, bytes));
    }

    CHECK(!std::filesystem::exists(texPath));

    auto loaded = ResourceManager::instance().load<IMaterial>(matPath);
    CHECK(loaded != nullptr);
    CHECK(ResourceManager::instance().getLoadState(matPath) == ResourceLoadState::Ready);

    CHECK(ResourceManager::instance().hasLoadFailed(texPath));
    CHECK(ResourceManager::instance().getLoadState(texPath) == ResourceLoadState::Failed);

    auto placeholder = ResourceManager::instance().getResource(texPath);
    CHECK(placeholder != nullptr);
    auto* tex = dynamic_cast<ITexture*>(placeholder.get());
    CHECK(tex != nullptr);
    CHECK(tex->getWidth() == 1u);
    CHECK(tex->getHeight() == 1u);

    cleanupDir();
}

TEST_CASE(create_handle_uses_manager_dep_graph)
{
    cleanupDir();
    std::filesystem::create_directories(kDir);
    resetManager();

    const std::string matPath = kDir + "/h.aymat";
    const std::string meshPath = kDir + "/h.aymesh";

    {
        auto mat = std::make_shared<Material>();
        mat->createDefault();
        std::vector<UInt8> bytes;
        CHECK(mat->saveToBinary(bytes));
        CHECK(writeBytes(matPath, bytes));
    }
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);
        mesh->_addForTestMaterialSlot("h.aymat");
        std::vector<UInt8> bytes;
        CHECK(mesh->saveToBinary(bytes));
        CHECK(writeBytes(meshPath, bytes));
    }

    auto handle = ResourceManager::instance().createHandle<IMesh>(meshPath);
    CHECK(handle != nullptr);
    auto mesh = handle->get();
    CHECK(mesh != nullptr);
    CHECK(ResourceManager::instance().isLoaded(matPath));

    cleanupDir();
}

TEST_SUITE_END
