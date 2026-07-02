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

TEST_SUITE_END
