#include "AYResource/AssetPath.h"
#include "AYTest.h"

#include <filesystem>
#include <fstream>

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

TEST_CASE(resolve_virtual_path_without_root_stays_logical)
{
    setAssetRoot("");
    const std::string resolved =
        resolveAssetPath("assets/meshes/hero.aymesh", "materials/hero.aymat");
    // Package/DB runtimes intentionally have no loose asset root. Preserve
    // the virtual path so it remains a valid ResourceManager lookup key.
    CHECK(resolved == "materials/hero.aymat"
          || resolved == "materials\\hero.aymat");
}

TEST_CASE(resolve_with_asset_root)
{
    setAssetRoot("content");
    const std::string resolved = resolveAssetPath("", "shaders/pbr.phoskia");
    CHECK(resolved == "content/shaders/pbr.phoskia"
          || resolved == "content\\shaders\\pbr.phoskia");
    setAssetRoot("");
}

TEST_CASE(resolve_virtual_path_searches_layered_roots_in_order)
{
    namespace fs = std::filesystem;
    const fs::path sandbox = fs::absolute("test_asset_path_layered_roots");
    const fs::path project = sandbox / "project";
    const fs::path cache = sandbox / "cache";
    fs::create_directories(project / "textures");
    fs::create_directories(cache / "textures");
    {
        std::ofstream out(project / "textures" / "project.png");
        out << "project";
    }
    {
        std::ofstream out(cache / "textures" / "cache.png");
        out << "cache";
    }

    setAssetRoots({project.string(), cache.string()});
    CHECK(assetRoots().size() == 2u);
    CHECK(fs::equivalent(resolveAssetPath({}, "textures/project.png"),
                         project / "textures" / "project.png"));
    CHECK(fs::equivalent(resolveAssetPath({}, "textures/cache.png"),
                         cache / "textures" / "cache.png"));

    setAssetRoot("");
    fs::remove_all(sandbox);
}

TEST_CASE(resolve_windows_absolute_base)
{
    const std::string resolved = resolveAssetPath(
        "C:/Temp/demo_cube.aymat", "demo_simple_lit.phoskia");
    CHECK(resolved == "C:/Temp/demo_simple_lit.phoskia"
          || resolved == "C:\\Temp\\demo_simple_lit.phoskia");
}

TEST_SUITE_END
