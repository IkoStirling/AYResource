#include "AYResource.h"
#include "AYTest.h"

// Consumer TU: only the public umbrella + propagated interface headers.
// Internal paths (assetsImpl / Loader / Converter) must not be required here.

using namespace ayt::resource;

TEST_SUITE(PublicApiSurfaceTests)

TEST_CASE(umbrella_header_compiles_without_internal_includes)
{
    static_assert(MAJOR_VERSION >= 1);
    ResourceManager& manager = ResourceManager::instance();
    (void)manager;
    CHECK(true);
}

TEST_SUITE_END
