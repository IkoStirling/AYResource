// AYTest_ResourcePipelineP6.cpp — P6: unload/trim do not notify; hot-reload does

#include "AYResource.h"
#include "AYMesh.h"
#include "AYTest.h"
#include "AYStorage/IStorageDatabase.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace ayt::resource;

namespace {

const std::string kDir = "ayresource_p6_tmp";

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
    ResourceManager::instance().setOnHotReload({});
    ResourceManager::instance().setAutoWatchLoadedResources(false);
    ResourceManager::instance().unloadAll();
}

void cleanupDir()
{
    std::error_code ec;
    std::filesystem::remove_all(kDir, ec);
}

} // namespace

TEST_SUITE(ResourcePipelineP6Tests)

TEST_CASE(unload_does_not_invoke_hot_reload_callback)
{
    cleanupDir();
    std::filesystem::create_directories(kDir);
    resetManager();

    const std::string meshPath = kDir + "/cube.aymesh";
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);
        std::vector<UInt8> bytes;
        CHECK(mesh->saveToBinary(bytes));
        CHECK(writeBytes(meshPath, bytes));
    }

    auto loaded = ResourceManager::instance().load<IMesh>(meshPath);
    CHECK(loaded != nullptr);
    CHECK(ResourceManager::instance().isLoaded(meshPath));

    int callbackCount = 0;
    ResourceManager::instance().setOnHotReload([&](const std::string&) {
        ++callbackCount;
    });

    ResourceManager::instance().unloadResource(meshPath);
    CHECK(!ResourceManager::instance().isLoaded(meshPath));
    CHECK(callbackCount == 0);

    // Caller-held shared_ptr keeps the L2 object alive after cache remove.
    CHECK(loaded != nullptr);
    CHECK(loaded->getVertexCount() > 0u);

    cleanupDir();
}

TEST_CASE(trim_cache_does_not_invoke_hot_reload_callback)
{
    cleanupDir();
    std::filesystem::create_directories(kDir);
    resetManager();

    const std::string meshPath = kDir + "/cube.aymesh";
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);
        std::vector<UInt8> bytes;
        CHECK(mesh->saveToBinary(bytes));
        CHECK(writeBytes(meshPath, bytes));
    }

    CHECK(ResourceManager::instance().load<IMesh>(meshPath) != nullptr);

    int callbackCount = 0;
    ResourceManager::instance().setOnHotReload([&](const std::string&) {
        ++callbackCount;
    });

    ResourceCache::Config cfg;
    cfg.maxMemoryBytes = 1; // force aggressive demote
    cfg.weakGraceSeconds = 0.0f;
    ResourceManager::instance().setCacheConfig(cfg);
    ResourceManager::instance().trimCache();

    CHECK(callbackCount == 0);
    cleanupDir();
}

TEST_CASE(hot_reload_path_still_notifies_after_l2_replace)
{
    cleanupDir();
    std::filesystem::create_directories(kDir);
    resetManager();
    ResourceManager::instance().setAutoWatchLoadedResources(false);

    const std::string meshPath = kDir + "/cube.aymesh";
    std::vector<UInt8> bytes;
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);
        CHECK(mesh->saveToBinary(bytes));
        CHECK(writeBytes(meshPath, bytes));
    }

    CHECK(ResourceManager::instance().load<IMesh>(meshPath) != nullptr);

    int callbackCount = 0;
    std::string callbackPath;
    ResourceManager::instance().setOnHotReload([&](const std::string& path) {
        ++callbackCount;
        callbackPath = path;
    });

    // Simulate watcher: _handleHotReload is private; public surface is
    // watch + update. Force via reload-equivalent by touching through
    // unload+load is NOT the notify path — use watchResource + update
    // after rewriting file (same as P2, debounce 0).
    ResourceManager::instance().setHotReloadDebounceSeconds(0.0f);
    ResourceManager::instance().setHotReloadPollIntervalSeconds(0.0f);
    ResourceManager::instance().watchResource(meshPath);

    // Rewrite with different payload so mtime/content changes.
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(2.0f);
        CHECK(mesh->saveToBinary(bytes));
    }
    // NTFS mtime granularity — brief sleep like P2.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(writeBytes(meshPath, bytes));

    ResourceManager::instance().update(0.0f);
    CHECK(callbackCount >= 1);
    CHECK(!callbackPath.empty());

    cleanupDir();
}

TEST_SUITE_END
