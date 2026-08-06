#include "AYResource.h"
#include "AYTest.h"

using namespace ayt::resource;

TEST_SUITE(ResourceBootstrapTests)

TEST_CASE(initialize_loaders_registers_core_types)
{
    CHECK(initializeLoaders());
    CHECK(areLoadersInitialized());

    CHECK(ResourceRegistry::getTypeFromExtension(".aymesh") == "Mesh");
    CHECK(ResourceRegistry::getTypeFromExtension(".aymat") == "Material");
    CHECK(ResourceRegistry::getTypeFromExtension(".aytex") == "Texture");

    auto meshLoader = ResourceRegistry::createLoader("Mesh");
    CHECK(meshLoader != nullptr);
    CHECK(meshLoader->getResourceType() == std::string("Mesh"));
}

// P3.x 刀 1 (2026-08-06) — verify the .aymask extension is wired to the
// SkeletonMaskLoader by the bootstrap. Without this registration,
// ResourceManager::load<ISkeletonMask>("x.aymask") returns nullptr and
// the AnimationSystem bridge falls back to fail-soft (no mask).
TEST_CASE(initialize_loaders_registers_skeletonmask)
{
    CHECK(initializeLoaders());
    CHECK(areLoadersInitialized());

    CHECK(ResourceRegistry::getTypeFromExtension(".aymask") == "SkeletonMask");

    auto loader = ResourceRegistry::createLoader("SkeletonMask");
    CHECK(loader != nullptr);
    CHECK(loader->getResourceType() == std::string("SkeletonMask"));
}

TEST_SUITE_END
