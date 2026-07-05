#include "AYAssetPath.h"
#include "AYTest.h"

using namespace ayt::resource;

TEST_SUITE(AssetPathTests)

TEST_CASE(resolve_relative_to_base_file)
{
    const std::string resolved =
        resolveAssetPath("assets/meshes/hero.aymesh", "materials/hero.aymat");
    CHECK(resolved == "assets/meshes/materials/hero.aymat"
          || resolved == "assets\\meshes\\materials\\hero.aymat");
}

TEST_CASE(resolve_with_asset_root)
{
    setAssetRoot("content");
    const std::string resolved = resolveAssetPath("", "shaders/pbr.phoskia");
    CHECK(resolved == "content/shaders/pbr.phoskia"
          || resolved == "content\\shaders\\pbr.phoskia");
    setAssetRoot("");
}

TEST_CASE(resolve_windows_absolute_base)
{
    const std::string resolved = resolveAssetPath(
        "C:/Temp/demo_cube.aymat", "demo_simple_lit.phoskia");
    CHECK(resolved == "C:/Temp/demo_simple_lit.phoskia"
          || resolved == "C:\\Temp\\demo_simple_lit.phoskia");
}

TEST_SUITE_END
