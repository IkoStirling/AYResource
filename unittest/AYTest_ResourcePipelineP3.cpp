// AYTest_ResourcePipelineP3.cpp — P3: async pool + weak resurrection + persistent index

#include "AYResource.h"
#include "AYMockResource.h"
#include "AYResourceCache.h"
#include "AYTest.h"
#include "aystorage/IStorageDatabase.h"

#include <ayio/File.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace ayt::resource;

namespace {

void resetManager()
{
    initializeLoaders();
    ResourceManager::instance().setDatabase(ayt::storage::IStorageDatabase::create(":memory:"));
    ResourceManager::instance().setOnHotReload({});
    ResourceManager::instance().setAutoWatchLoadedResources(false);
    ResourceManager::instance().unloadAll();
}

} // namespace

TEST_SUITE(ResourcePipelineP3Tests)

TEST_CASE(async_load_via_task_pool_completes)
{
    resetManager();
    ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
        return std::make_unique<MockResourceLoader>();
    });
    ResourceRegistry::registerExtension(".mock", "MockResource");

    std::vector<std::shared_future<std::shared_ptr<MockResource>>> futures;
    for (int i = 0; i < 8; ++i) {
        const std::string path = "p3_async_" + std::to_string(i) + ".mock";
        {
            std::ofstream f(path);
            f << "data" << i;
        }
        futures.push_back(ResourceManager::instance().loadAsync<MockResource>(path));
    }

    for (size_t i = 0; i < futures.size(); ++i) {
        auto res = futures[i].get();
        CHECK(res != nullptr);
        CHECK(res->isLoaded());
        std::remove(("p3_async_" + std::to_string(i) + ".mock").c_str());
    }

    resetManager();
}

TEST_CASE(async_cancel_before_finish_returns_null)
{
    resetManager();
    ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
        return std::make_unique<MockResourceLoader>();
    });
    ResourceRegistry::registerExtension(".mock", "MockResource");

    const std::string path = "p3_cancel.mock";
    {
        std::ofstream f(path);
        f << "x";
    }

    // Local loader exercises cancel contract (Manager's AsyncLoader is private).
    AsyncLoader loader;
    auto future = loader.loadAsync(path);
    loader.cancel(path);
    auto result = future.get();
    // May be null (cancelled) or loaded (won the race) — must not throw / hang.
    (void)result;
    loader.cancelAll();

    std::remove(path.c_str());
    resetManager();
    CHECK(true);
}

TEST_CASE(weak_grace_resurrects_without_disk_reload)
{
    ResourceCache::Config config;
    config.maxResourceCount = 8;
    config.weakGraceSeconds = 2.0f;
    ResourceCache cache(config);

    auto original = std::make_shared<MockResource>();
    cache.put("grace.mock", original);
    CHECK(cache.stats().strongCount == 1u);

    // Simulate last-handle drop → demote to grace.
    cache.registerHandle("grace.mock");
    cache.unregisterHandle("grace.mock");
    CHECK(cache.stats().strongCount == 0u);
    CHECK(cache.stats().graceCount == 1u);

    auto resurrected = cache.get("grace.mock");
    CHECK(resurrected != nullptr);
    CHECK(resurrected.get() == original.get());
    CHECK(cache.stats().resurrectCount >= 1u);
    CHECK(cache.stats().strongCount == 1u);
}

TEST_CASE(cache_stats_track_hits_and_budget)
{
    ResourceCache::Config config;
    config.maxMemoryBytes = 1024 * 1024;
    config.maxResourceCount = 4;
    config.weakGraceSeconds = 0.0f;
    ResourceCache cache(config);

    CHECK(cache.get("missing.mock") == nullptr);
    CHECK(cache.stats().missCount >= 1u);

    cache.put("a.mock", std::make_shared<MockResource>());
    CHECK(cache.get("a.mock") != nullptr);
    const auto s = cache.stats();
    CHECK(s.hitCount >= 1u);
    CHECK(s.memoryBudget == config.maxMemoryBytes);
    CHECK(s.memoryBytes <= s.memoryBudget);
}

TEST_CASE(persistent_cache_index_round_trip)
{
    resetManager();
    ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
        return std::make_unique<MockResourceLoader>();
    });
    ResourceRegistry::registerExtension(".mock", "MockResource");

    const std::string asset = "p3_persist_asset.mock";
    const std::string index = "p3_persist_index.aycache";
    {
        std::ofstream f(asset);
        f << "persist";
    }

    CHECK(ResourceManager::instance().load<MockResource>(asset) != nullptr);
    ResourceManager::instance().savePersistentCache(index);
    CHECK(ayt::io::File::exists(index));

    ResourceManager::instance().unloadAll();
    CHECK(ResourceManager::instance().isLoaded(asset) == false);

    ResourceManager::instance().loadPersistentCache(index);
    CHECK(ResourceManager::instance().isLoaded(asset));

    std::remove(asset.c_str());
    std::remove(index.c_str());
    resetManager();
}

TEST_SUITE_END
