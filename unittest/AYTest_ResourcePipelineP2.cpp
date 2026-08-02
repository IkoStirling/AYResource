// AYTest_ResourcePipelineP2.cpp — P2: hot-reload L2 invalidate + reload
//
// Covers:
//   - Manager auto-watch after load
//   - file touch → update → L2 instance replaced + user callback
//   - ResourceHandle::get() picks up the new L2 instance

#include "AYResource.h"
#include "AYMesh.h"
#include "AYTest.h"
#include "aystorage/IStorageDatabase.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace ayt::resource;

namespace {

const std::string kDir = "ayresource_p2_tmp";

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
    ResourceManager::instance().setAutoWatchLoadedResources(true);
    ResourceManager::instance().setHotReloadDebounceSeconds(0.0f);
    ResourceManager::instance().setHotReloadPollIntervalSeconds(0.0f);
    ResourceManager::instance().unloadAll();
}

void cleanupDir()
{
    std::error_code ec;
    std::filesystem::remove_all(kDir, ec);
}

void touchRewrite(const std::string& path, const std::vector<UInt8>& data)
{
    // NTFS mtime granularity can be ~1s; sleep so fallback detects change.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(writeBytes(path, data));
}

} // namespace

TEST_SUITE(ResourcePipelineP2Tests)

TEST_CASE(hot_reload_replaces_l2_and_notifies)
{
    cleanupDir();
    std::filesystem::create_directories(kDir);
    resetManager();

    const std::string meshPath = kDir + "/cube.aymesh";
    std::vector<UInt8> bytesA;
    std::vector<UInt8> bytesB;
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);
        CHECK(mesh->saveToBinary(bytesA));
        CHECK(writeBytes(meshPath, bytesA));
    }
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(2.0f);
        CHECK(mesh->saveToBinary(bytesB));
    }

    auto first = ResourceManager::instance().load<IMesh>(meshPath);
    CHECK(first != nullptr);
    const UInt32 firstVerts = first->getVertexCount();

    std::string callbackPath;
    int callbackCount = 0;
    ResourceManager::instance().setOnHotReload([&](const std::string& path) {
        callbackPath = path;
        ++callbackCount;
    });

    // Force immediate debounce for the unit test (production uses ~100ms).
    // Access watcher indirectly: update after touch with auto-watch already on.
    touchRewrite(meshPath, bytesB);

    // Pump a few times so FileWatcher and/or mtime fallback settle.
    for (int i = 0; i < 5; ++i) {
        ResourceManager::instance().update(0.0f);
        if (callbackCount > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK(callbackCount >= 1);
    CHECK(callbackPath.find("cube.aymesh") != std::string::npos);

    auto second = ResourceManager::instance().getResource(meshPath);
    CHECK(second != nullptr);
    CHECK(second.get() != first.get());
    CHECK(ResourceManager::instance().getLoadState(meshPath) == ResourceLoadState::Ready);

    // Cube topology is identical across sizes; still a valid reload.
    CHECK(std::dynamic_pointer_cast<IMesh>(second)->getVertexCount() == firstVerts);

    cleanupDir();
    resetManager();
}

TEST_CASE(resource_handle_refetches_after_hot_reload)
{
    cleanupDir();
    std::filesystem::create_directories(kDir);
    resetManager();

    const std::string meshPath = kDir + "/h.aymesh";
    std::vector<UInt8> bytes;
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);
        CHECK(mesh->saveToBinary(bytes));
        CHECK(writeBytes(meshPath, bytes));
    }

    auto handle = ResourceManager::instance().createHandle<IMesh>(meshPath);
    auto first = handle->get();
    CHECK(first != nullptr);

    int callbackCount = 0;
    ResourceManager::instance().setOnHotReload([&](const std::string&) {
        ++callbackCount;
    });

    touchRewrite(meshPath, bytes);
    for (int i = 0; i < 8; ++i) {
        ResourceManager::instance().update(0.0f);
        if (callbackCount > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(callbackCount >= 1);

    auto second = handle->get();
    CHECK(second != nullptr);
    CHECK(second.get() != first.get());

    cleanupDir();
    resetManager();
}

TEST_SUITE_END
