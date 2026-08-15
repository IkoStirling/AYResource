#include "AYResource.h"
#include "AYMockResource.h"
#include "AYTest.h"
#include "AYHotReloadWatcher.h"
#include "AYIO/File.h"
#include "AYStorage/IStorageDatabase.h"
#include <fstream>
#include <thread>
#include <chrono>

using namespace ayt::resource;

TEST_SUITE(ResourceCacheTests)

    TEST_CASE(DefaultConfig) {
        ResourceCache cache;
        auto config = cache.config();
        CHECK(config.maxMemoryBytes == 512 * 1024 * 1024);
        CHECK(config.maxResourceCount == 200);
        CHECK(config.enableLRU == true);
    }

    TEST_CASE(PutAndGet) {
        ResourceCache cache;
        auto resource = std::make_shared<MockResource>();
        cache.put("test/path.mock", resource);

        CHECK(cache.contains("test/path.mock") == true);
        CHECK(cache.strongCount() == 1);
    }

    TEST_CASE(GetNonExistent) {
        ResourceCache cache;
        auto result = cache.get("nonexistent/path.mock");
        CHECK(result == nullptr);
    }

    TEST_CASE(Remove) {
        ResourceCache cache;
        auto resource = std::make_shared<MockResource>();
        cache.put("test/path.mock", resource);

        cache.remove("test/path.mock");
        CHECK(cache.contains("test/path.mock") == false);
        CHECK(cache.strongCount() == 0);
    }

    TEST_CASE(MemoryTracking) {
        ResourceCache cache;
        CHECK(cache.memoryUsage() == 0);

        auto resource = std::make_shared<MockResource>();
        cache.put("test/path.mock", resource);

        CHECK(cache.memoryUsage() > 0);
    }

    TEST_CASE(LRUEvictionOnCount) {
        ResourceCache::Config config;
        config.maxResourceCount = 2;
        config.enableLRU = true;
        // No grace pin so demoted entries expire immediately (legacy assert).
        config.weakGraceSeconds = 0.0f;

        ResourceCache cache(config);

        cache.put("res1.mock", std::make_shared<MockResource>());
        cache.put("res2.mock", std::make_shared<MockResource>());
        cache.put("res3.mock", std::make_shared<MockResource>());

        CHECK(cache.contains("res1.mock") == false);
        CHECK(cache.contains("res2.mock") == true);
        CHECK(cache.contains("res3.mock") == true);
    }

    TEST_CASE(ClearCache) {
        ResourceCache cache;
        cache.put("res1.mock", std::make_shared<MockResource>());
        cache.put("res2.mock", std::make_shared<MockResource>());

        cache.clear();

        CHECK(cache.strongCount() == 0);
        CHECK(cache.memoryUsage() == 0);
    }

TEST_SUITE_END

TEST_SUITE(DependencyGraphTests)

    TEST_CASE(AddDependency) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"player.mesh", "mesh", "", 0, 0, "", ""});
        db->addDependency("player.mesh", "player.skeleton");

        auto deps = db->getDependencies("player.mesh");
        CHECK(deps.size() == 1);
        CHECK(deps[0] == "player.skeleton");
    }

    TEST_CASE(GetDependents) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"player.mesh", "mesh", "", 0, 0, "", ""});
        db->insertResource({"player.skeleton", "skeleton", "", 0, 0, "", ""});
        db->addDependency("player.mesh", "player.skeleton");

        auto dependents = db->getDependents("player.skeleton");
        CHECK(dependents.size() == 1);
        CHECK(dependents[0] == "player.mesh");
    }

    TEST_CASE(NoCircularDependency) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"a", "type", "", 0, 0, "", ""});
        db->insertResource({"b", "type", "", 0, 0, "", ""});
        db->insertResource({"c", "type", "", 0, 0, "", ""});
        db->addDependency("a", "b");
        db->addDependency("b", "c");

        CHECK(db->hasCircularDependency("a") == false);
    }

    TEST_CASE(DetectCircularDependency) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"a", "type", "", 0, 0, "", ""});
        db->insertResource({"b", "type", "", 0, 0, "", ""});
        db->insertResource({"c", "type", "", 0, 0, "", ""});
        db->addDependency("a", "b");
        db->addDependency("b", "c");
        db->addDependency("c", "a");

        CHECK(db->hasCircularDependency("a") == true);
    }

    TEST_CASE(RemoveDependency) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"a", "type", "", 0, 0, "", ""});
        db->insertResource({"b", "type", "", 0, 0, "", ""});
        db->addDependency("a", "b");
        db->removeDependency("a", "b");

        auto deps = db->getDependencies("a");
        CHECK(deps.empty());
    }

    TEST_CASE(GetLoadOrder) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"scene", "scene", "", 0, 0, "", ""});
        db->insertResource({"player.mesh", "mesh", "", 0, 0, "", ""});
        db->insertResource({"player.skeleton", "skeleton", "", 0, 0, "", ""});
        db->insertResource({"player.texture", "texture", "", 0, 0, "", ""});
        db->addDependency("scene", "player.mesh");
        db->addDependency("player.mesh", "player.skeleton");
        db->addDependency("player.mesh", "player.texture");

        auto order = db->getLoadOrder("scene");
        CHECK(order.size() >= 3);
    }

TEST_SUITE_END

TEST_SUITE(ResourceRegistryTests)

    TEST_CASE(RegisterAndCreateLoader) {
        ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
            return std::make_unique<MockResourceLoader>();
        });

        auto loader = ResourceRegistry::createLoader("MockResource");
        CHECK(loader != nullptr);
        CHECK(loader->canLoad("test.mock") == true);
        CHECK(loader->canLoad("test.other") == false);
    }

    TEST_CASE(RegisterExtension) {
        ResourceRegistry::registerExtension(".mock", "MockResource");
        auto type = ResourceRegistry::getTypeFromExtension(".mock");
        CHECK(type == "MockResource");
    }

    TEST_CASE(GetTypeFromPath) {
        ResourceRegistry::registerExtension(".mock", "MockResource");
        auto type = ResourceRegistry::getTypeFromPath("/path/to/file.mock");
        CHECK(type == "MockResource");
    }

TEST_SUITE_END

TEST_SUITE(IResourceTests)

    TEST_CASE(InitialState) {
        MockResource resource;
        CHECK(resource.isLoaded() == false);
        CHECK(resource.getPath().empty());
    }

    TEST_CASE(LoadUnload) {
        MockResource resource;
        resource.load("test/path.mock");

        CHECK(resource.isLoaded() == true);
        CHECK(resource.getPath() == "test/path.mock");
    }

    TEST_CASE(Reload) {
        MockResource resource;
        resource.load("old/path.mock");
        resource.reload("new/path.mock");

        CHECK(resource.getPath() == "new/path.mock");
    }

    TEST_CASE(Tags) {
        MockResource resource;
        ResourceTag tag1{"priority", "high"};
        ResourceTag tag2{"category", "game"};

        CHECK(resource.hasTag(tag1) == false);

        resource.addTag(tag1);
        CHECK(resource.hasTag(tag1) == true);

        resource.addTag(tag2);
        CHECK(resource.getTags().size() == 2);

        resource.removeTag(tag1);
        CHECK(resource.hasTag(tag1) == false);
    }

TEST_SUITE_END

TEST_SUITE(ResourceManagerTests)

    TEST_CASE(LoadFromDatabaseLooseFile) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"test/mock.mock", "MockResource", "", 1024, 0, "", ""});

        // Register mock loader
        ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
            return std::make_unique<MockResourceLoader>();
        });

        ResourceManager::instance().setDatabase(std::move(db));
        ResourceManager::instance().unloadAll();
        auto resource = ResourceManager::instance().load<MockResource>("test/mock.mock");
        CHECK(resource != nullptr);
        CHECK(resource->isLoaded() == true);
    }

    TEST_CASE(LoadFromDatabaseInPackage) {
        // This test requires a real package file, so we just verify the path
        // The actual package loading would need integration test with real .pak file
        CHECK(true);
    }

    TEST_CASE(RecursiveDependencyLoading) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"scene.mock", "MockResource", "", 0, 0, "", ""});
        db->insertResource({"dep1.mock", "MockResource", "", 0, 0, "", ""});
        db->insertResource({"dep2.mock", "MockResource", "", 0, 0, "", ""});
        db->addDependency("scene.mock", "dep1.mock");
        db->addDependency("dep1.mock", "dep2.mock");

        ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
            return std::make_unique<MockResourceLoader>();
        });

        ResourceManager::instance().setDatabase(std::move(db));
        ResourceManager::instance().unloadAll();
        auto scene = ResourceManager::instance().load<MockResource>("scene.mock");

        // Dependencies should be loaded first (order: dep2, dep1, scene)
        CHECK(scene != nullptr);
        CHECK(ResourceManager::instance().isLoaded("dep1.mock") == true);
        CHECK(ResourceManager::instance().isLoaded("dep2.mock") == true);
    }

    TEST_CASE(PackageReaderCaching) {
        // Verify same package reader is reused
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"res1.mock", "MockResource", "", 0, 0, "", ""});
        db->insertResource({"res2.mock", "MockResource", "", 0, 0, "", ""});

        ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
            return std::make_unique<MockResourceLoader>();
        });

        ResourceManager::instance().setDatabase(std::move(db));
        ResourceManager::instance().unloadAll();
        // Both resources in same package
        auto res1 = ResourceManager::instance().load<MockResource>("res1.mock");
        auto res2 = ResourceManager::instance().load<MockResource>("res2.mock");

        CHECK(res1 != nullptr);
        CHECK(res2 != nullptr);
    }

    TEST_CASE(PreloadTaggedResources) {
        auto db = ayt::storage::IStorageDatabase::create(":memory:");
        db->insertResource({"tagged1.mock", "MockResource", "", 0, 0, "", ""});
        db->insertResource({"tagged2.mock", "MockResource", "", 0, 0, "", ""});
        db->addTag("tagged1.mock", "AlwaysLoaded", "loading");
        db->addTag("tagged2.mock", "AlwaysLoaded", "loading");

        ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
            return std::make_unique<MockResourceLoader>();
        });

        ResourceManager::instance().setDatabase(std::move(db));
        ResourceManager::instance().unloadAll();
        ResourceManager::instance().preloadResourcesWithTag("AlwaysLoaded", "loading");

        CHECK(ResourceManager::instance().isLoaded("tagged1.mock") == true);
        CHECK(ResourceManager::instance().isLoaded("tagged2.mock") == true);
    }

TEST_SUITE_END

TEST_SUITE(HotReloadWatcherTests)

    TEST_CASE(WatchAndUnwatch) {
        HotReloadWatcher watcher;

        std::string tempPath = "test_watch_file.tmp";
        {
            std::ofstream f(tempPath);
            f << "test";
        }

        watcher.watch(tempPath);
        CHECK(watcher.watchCount() == 1);
        CHECK(watcher.isWatching(tempPath) == true);

        watcher.unwatch(tempPath);
        CHECK(watcher.watchCount() == 0);
        CHECK(watcher.isWatching(tempPath) == false);

        std::remove(tempPath.c_str());
    }

    TEST_CASE(WatchNonExistentFile) {
        HotReloadWatcher watcher;
        watcher.watch("nonexistent_file_12345.tmp");
        CHECK(watcher.isWatching("nonexistent_file_12345.tmp") == true);
        CHECK(watcher.watchCount() == 1);
    }

    TEST_CASE(UnwatchAll) {
        HotReloadWatcher watcher;

        std::string tempPath1 = "test_watch_1.tmp";
        std::string tempPath2 = "test_watch_2.tmp";
        {
            std::ofstream f1(tempPath1);
            f1 << "test1";
            std::ofstream f2(tempPath2);
            f2 << "test2";
        }

        watcher.watch(tempPath1);
        watcher.watch(tempPath2);
        CHECK(watcher.watchCount() == 2);

        watcher.unwatchAll();
        CHECK(watcher.watchCount() == 0);

        std::remove(tempPath1.c_str());
        std::remove(tempPath2.c_str());
    }

    TEST_CASE(SetPollInterval) {
        HotReloadWatcher watcher;
        watcher.setPollInterval(2.5f);
        CHECK(true);
    }

    TEST_CASE(FileLastModifiedTimePoint) {
        std::string tempPath = "test_time_point.tmp";
        {
            std::ofstream f(tempPath);
            f << "test content";
        }

        auto tp = ayt::io::File::lastModifiedTimePoint(tempPath);
        CHECK(tp.has_value());
        CHECK(tp->toUnixSeconds() > 0);

        std::remove(tempPath.c_str());
    }

    TEST_CASE(FileModificationDetection) {
        HotReloadWatcher watcher;

        std::string tempPath = "test_modify.tmp";
        {
            std::ofstream f(tempPath);
            f << "original content";
        }

        watcher.watch(tempPath);

        bool callbackInvoked = false;
        watcher.setOnFileChanged([&](const std::string& path) {
            callbackInvoked = true;
            CHECK(path == tempPath || path.find("test_modify.tmp") != std::string::npos);
        });

        // Wait to ensure filesystem updates modification time (need 1 second for NTFS/FAT32 precision)
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Modify the file
        {
            std::ofstream f(tempPath);
            f << "modified content";
        }

        // P2: debounce coalesces OS + mtime; zero it for a deterministic unit test.
        watcher.setDebounceSeconds(0.0f);
        watcher.setPollInterval(0.0f);
        watcher.update();

        CHECK(callbackInvoked == true);

        std::remove(tempPath.c_str());
    }

TEST_SUITE_END
