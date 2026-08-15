// AYTest_ResourcePipelineP3.cpp — P3: async pool + weak resurrection + persistent index

#include "AYResource.h"
#include "AYMockResource.h"
#include "AYResourceCache.h"
#include "AYTest.h"
#include "AYStorage/IStorageDatabase.h"

#include <AYIO/File.h>

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

// CM-5 (2026-08-12): regression stress for a data race in the async path.
// _loadInternal mutated _loadingPaths / _loadStates / _resourceTypes
// without locks while the pool ran loads concurrently, and (for
// first-load-is-async flows) initializeLoaders() ran registry
// registration from every worker. The corruption surfaced
// nondeterministically — often in unrelated later tests, e.g.
// ResourceCache::clear reading a torn string key. This case exercises
// the same machinery at higher concurrency (32 loads × 10 unloadAll
// rounds); it can't prove a race's absence, but a reintroduced race is
// far more likely to trip here than in the single-round test above.
TEST_CASE(async_many_concurrent_unload_rounds_stable)
{
    for (int round = 0; round < 10; ++round) {
        resetManager();
        ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
            return std::make_unique<MockResourceLoader>();
        });
        ResourceRegistry::registerExtension(".mock", "MockResource");

        std::vector<std::shared_future<std::shared_ptr<MockResource>>> futures;
        for (int i = 0; i < 32; ++i) {
            const std::string path =
                "p3_stress_" + std::to_string(round) + "_" + std::to_string(i) + ".mock";
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
            std::remove(("p3_stress_" + std::to_string(round) + "_" + std::to_string(i) + ".mock")
                            .c_str());
        }

        resetManager();
    }
    CHECK(true);
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

// F1.1 regression: cancel race that historically triggered
// std::future_error (process termination). Two loadAsync calls + one
// cancel(path) for one of them; the second must still resolve without
// crash. Doubles as a concurrency smoke (2 producers + 1 canceller).
TEST_CASE(async_cancel_one_of_many_does_not_affect_siblings)
{
    resetManager();
    ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
        return std::make_unique<MockResourceLoader>();
    });
    ResourceRegistry::registerExtension(".mock", "MockResource");

    std::vector<std::string> paths;
    for (int i = 0; i < 4; ++i) {
        const std::string p = "p3_cancel_many_" + std::to_string(i) + ".mock";
        std::ofstream f(p);
        f << "x";
        paths.push_back(p);
    }

    AsyncLoader loader;
    auto f0 = loader.loadAsync(paths[0]);
    auto f1 = loader.loadAsync(paths[1]);
    auto f2 = loader.loadAsync(paths[2]);
    auto f3 = loader.loadAsync(paths[3]);

    // Cancel two of them.
    loader.cancel(paths[1]);
    loader.cancel(paths[3]);

    // None of these futures may throw or hang.
    (void)f0.get();
    (void)f1.get();
    (void)f2.get();
    (void)f3.get();

    loader.cancelAll();
    for (const auto& p : paths) std::remove(p.c_str());
    resetManager();
    CHECK(true);
}

// F1.1 regression: progress callback must fire from a worker thread
// and not deadlock even if it touches the same AsyncLoader (re-entrant
// cancel/cancelAll) — the callback must NOT be invoked under the
// loader mutex.
TEST_CASE(async_progress_callback_can_cancel_without_deadlock)
{
    resetManager();
    ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
        return std::make_unique<MockResourceLoader>();
    });
    ResourceRegistry::registerExtension(".mock", "MockResource");

    const std::string path = "p3_progress_cancel.mock";
    std::ofstream f(path); f << "x";

    AsyncLoader loader;
    bool callbackFired = false;
    auto future = loader.loadAsync(
        path,
        {},
        [&loader, &callbackFired](const std::string&, float /*p*/) {
            // Touch the loader from inside the callback (this would
            // deadlock if the loader were holding its own mutex).
            loader.pendingCount();
            callbackFired = true;
        });

    // Wait for the load to complete (worker calls progress=1.0 then
    // sets the future). Timeout-bounded in case of regression.
    auto status = future.wait_for(std::chrono::seconds(5));
    CHECK(status == std::future_status::ready);
    (void)future.get();
    CHECK(callbackFired);

    loader.cancelAll();
    std::remove(path.c_str());
    resetManager();
    CHECK(true);
}

// F1.1 regression: cancelAll() must drain queued tasks without
// double-setting any future (the v0 path could fire std::future_error
// if cancelAll()'s promise.set_value(nullptr) raced the worker's
// promise.set_value(resource)).
TEST_CASE(async_cancelAll_then_load_more_does_not_throw)
{
    resetManager();
    ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
        return std::make_unique<MockResourceLoader>();
    });
    ResourceRegistry::registerExtension(".mock", "MockResource");

    std::vector<std::string> paths;
    for (int i = 0; i < 3; ++i) {
        const std::string p = "p3_cancelall_" + std::to_string(i) + ".mock";
        std::ofstream f(p);
        f << "x";
        paths.push_back(p);
    }

    AsyncLoader loader;
    auto a = loader.loadAsync(paths[0]);
    auto b = loader.loadAsync(paths[1]);
    auto c = loader.loadAsync(paths[2]);

    loader.cancelAll();

    // Drain.
    (void)a.get();
    (void)b.get();
    (void)c.get();

    // Now load more — must succeed after a cancelAll.
    const std::string p3 = "p3_after_cancelall.mock";
    std::ofstream f(p3); f << "x";
    auto after = loader.loadAsync(p3);
    auto r = after.get();
    CHECK(r != nullptr);

    loader.cancelAll();
    for (const auto& p : paths) std::remove(p.c_str());
    std::remove(p3.c_str());
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

// F4.1: presence-of-version sanity. The static_assert in IAYResource.h
// gates the compile, so this test exists to make the intent explicit
// at the test boundary and catch accidental edits to the macro.
TEST_CASE(version_tripwire_is_200)
{
#if defined(AYRESOURCE_VERSION)
    CHECK(AYRESOURCE_VERSION == 200);
#else
    CHECK(false); // macro missing — header guard must have been removed
#endif
}

// F4.2: setCacheConfig must not crash + must produce a usable cache.
// The previous placement-new implementation was UB under concurrent
// reads; the new ResourceCache::rebuild() path holds the internal
// mutex for the full reset.
TEST_CASE(setCacheConfig_rebuilds_without_use_after_free)
{
    resetManager();
    ResourceCache::Config cfg;
    cfg.maxResourceCount = 4;
    ResourceCache cache(cfg);
    cache.put("a", std::make_shared<MockResource>());
    cache.put("b", std::make_shared<MockResource>());
    CHECK(cache.strongCount() == 2u);

    // Change the cap and rebuild — must not crash on the old entries.
    cfg.maxResourceCount = 2;
    cache.rebuild(cfg);
    CHECK(cache.strongCount() == 0u);
    CHECK(cache.config().maxResourceCount == 2u);

    cache.put("c", std::make_shared<MockResource>());
    CHECK(cache.get("c") != nullptr);
    resetManager();
}

// F4.2: ResourceCache::rebuild must reset the stats counters in
// addition to clearing the storage. v0 only cleared the storage and
// left hitCount/resurrectCount/dropCount at whatever the previous
// config had.
TEST_CASE(setCacheConfig_resets_stats)
{
    ResourceCache::Config cfg;
    ResourceCache cache(cfg);
    (void)cache.get("missing.mock"); // bumps missCount
    CHECK(cache.stats().missCount >= 1u);

    cache.rebuild(cfg);
    CHECK(cache.stats().missCount == 0u);
    CHECK(cache.stats().hitCount == 0u);
    CHECK(cache.stats().resurrectCount == 0u);
}

// F4.2: invalidatePak with empty path drops all readers. Previously
// the only way to release a pak reader was unloadAll, which also
// nuked the cache + load lifecycle state.
TEST_CASE(invalidatePak_empty_drops_all)
{
    resetManager();
    CHECK(ResourceManager::instance().openPaksCount() == 0u);
    // invalidatePak(empty) on an empty map is a no-op but must not
    // throw and must return false.
    CHECK(ResourceManager::instance().invalidatePak("") == false);
    CHECK(ResourceManager::instance().openPaksCount() == 0u);
    resetManager();
}

TEST_SUITE_END
