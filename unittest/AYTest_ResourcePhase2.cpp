#include "AYResource.h"
#include "AYMockResource.h"
#include "AYTest.h"
#include "AYLooseDependency.h"
#include "IAYStorageDatabase.h"

#include <fstream>
#include <filesystem>

using namespace ayt::resource;

TEST_SUITE(ResourceManagerPhase2Tests)

TEST_CASE(load_loose_dependencies_from_aydep_sidecar)
{
    const std::string dir = "ayresource_phase2_tmp";
    const std::string scenePath = dir + "/scene.mock";
    const std::string depPath = dir + "/dep.mock";
    const std::string sidecarPath = dir + "/scene.aydep.json";

    std::filesystem::create_directories(dir);
    {
        std::ofstream dep(depPath);
        dep << "dep";
        std::ofstream scene(scenePath);
        scene << "scene";
        std::ofstream sidecar(sidecarPath);
        sidecar << "{\n"
                   "  \"dependencies\": [\n"
                   "    {\"from\": \"scene.mock\", \"to\": \"dep.mock\"}\n"
                   "  ]\n"
                   "}\n";
    }

    ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
        return std::make_unique<MockResourceLoader>();
    });
    ResourceRegistry::registerExtension(".mock", "MockResource");

    ResourceManager::instance().setDatabase(ayt::storage::IStorageDatabase::create(":memory:"));
    ResourceManager::instance().unloadAll();

    auto scene = ResourceManager::instance().load<MockResource>(scenePath);
    CHECK(scene != nullptr);
    CHECK(ResourceManager::instance().isLoaded(depPath));

    std::remove(scenePath.c_str());
    std::remove(depPath.c_str());
    std::remove(sidecarPath.c_str());
}

TEST_CASE(async_load_uses_resource_manager_path)
{
    ResourceRegistry::registerLoader("MockResource", +[]() -> std::unique_ptr<IResourceLoader> {
        return std::make_unique<MockResourceLoader>();
    });
    ResourceRegistry::registerExtension(".mock", "MockResource");

    ResourceManager::instance().setDatabase(ayt::storage::IStorageDatabase::create(":memory:"));
    ResourceManager::instance().unloadAll();

    auto future = ResourceManager::instance().loadAsync<MockResource>("async.mock");
    auto resource = future.get();
    CHECK(resource != nullptr);
    CHECK(resource->isLoaded());
    CHECK(ResourceManager::instance().isLoaded("async.mock"));
}

TEST_CASE(conversion_result_from_json_parses_dependencies)
{
    const std::string json =
        "{\n"
        "  \"dependencies\": [\n"
        "    {\"from\": \"a.aymat\", \"to\": \"textures/t.ex\"}\n"
        "  ]\n"
        "}\n";

    const ConversionResult parsed = ConversionResult::fromJson(json);
    CHECK(parsed.dependencies.size() == 1);
    CHECK(parsed.dependencies[0].from == std::string("a.aymat"));
    CHECK(parsed.dependencies[0].to == std::string("textures/t.ex"));
}

TEST_SUITE_END
