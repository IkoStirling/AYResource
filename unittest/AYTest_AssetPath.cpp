#include "AYResource/AssetPath.h"
#include "AYTest.h"

using namespace ayt::resource;

TEST_SUITE(AssetPathTests)

TEST_CASE(resolve_relative_to_base_file_sibling)
{
    // Bare filename next to the referring asset (legacy / demo materials).
    const std::string resolved = resolveAssetPath(
        "C:/Temp/demo_cube.aymat", "demo_simple_lit.phoskia");
    CHECK(resolved == "C:/Temp/demo_simple_lit.phoskia"
          || resolved == "C:\\Temp\\demo_simple_lit.phoskia");
}

TEST_CASE(resolve_virtual_path_uses_asset_root)
{
    setAssetRoot("D:/cache/assets");
    const std::string resolved =
        resolveAssetPath("D:/cache/assets/meshes/hero.aymesh",
                         "materials/hero.aymat");
    CHECK(resolved == "D:/cache/assets/materials/hero.aymat"
          || resolved == "D:\\cache\\assets\\materials\\hero.aymat");
    setAssetRoot("");
}

TEST_CASE(resolve_virtual_path_without_root_falls_back_to_base_dir)
{
    setAssetRoot("");
    const std::string resolved =
        resolveAssetPath("assets/meshes/hero.aymesh", "materials/hero.aymat");
    // Without asset root, keep legacy join(baseDir, ref) behavior.
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
