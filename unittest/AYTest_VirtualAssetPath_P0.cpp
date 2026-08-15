#include "AYTest.h"
#include "AYResource/VirtualAssetPath.h"
#include "AYResource/Converter/MaterialConverter.h"
#include "AYResource/IntermediateAsset.h"
#include "AYResource/assetsImpl/Material.h"

#include <AYIO/File.h>
#include <filesystem>
#include <string>

using namespace ayt::resource;

TEST_SUITE(VirtualAssetPath_P0)

TEST_CASE(material_path_contract)
{
    CHECK(makeMaterialVirtualPath("Sour", 0) ==
          std::string("materials/Sour_material_0.aymat"));
    CHECK(makeMaterialVirtualPath("Sour", 38) ==
          std::string("materials/Sour_material_38.aymat"));
}

TEST_CASE(texture_stem_and_path_contract)
{
    CHECK(makeTextureStemFromSourcePath("tex/skin.png") ==
          std::string("tex_skin"));
    CHECK(makeTextureStemFromSourcePath("skin.png") ==
          std::string("skin"));
    CHECK(makeTextureStemFromSourcePath("D:\\Models\\skin.png") ==
          std::string("skin"));

    CHECK(makeTextureVirtualPath("tex_skin", "_d") ==
          std::string("textures/tex_skin_d.aytex"));
    CHECK(makeTextureVirtualPathFromSource("tex/skin.png") ==
          std::string("textures/tex_skin_d.aytex"));
}

TEST_CASE(material_convertAll_emits_one_file_per_material)
{
    namespace fs = std::filesystem;
    const fs::path outDir =
        fs::temp_directory_path() / "ay_p0_mat_convertAll";
    fs::remove_all(outDir);
    fs::create_directories(outDir / "materials");

    std::vector<MaterialData> mats(2);
    mats[0].name = "material_0";
    mats[0].shader = "shaders/pbr.phoskia";
    {
        Param p;
        p.name = "baseColorTexture";
        p.type = MaterialParamType::Texture2D;
        p.texturePath = makeTextureVirtualPath("skin", "_d");
        mats[0].parameters.push_back(p);
    }
    mats[1].name = "material_1";
    mats[1].shader = "shaders/pbr.phoskia";

    MaterialConverter conv;
    conv.setOutputDir(outDir.string());
    auto results = conv.convertAll(mats, "Hero");

    CHECK(results.size() == 2u);
    CHECK(results[0].path == makeMaterialVirtualPath("Hero", 0));
    CHECK(results[1].path == makeMaterialVirtualPath("Hero", 1));

    const std::string path0 = (outDir / results[0].path).string();
    const std::string path1 = (outDir / results[1].path).string();
    CHECK(ayt::io::File::exists(path0));
    CHECK(ayt::io::File::exists(path1));

    // Runtime loader must accept the cooked file (single-material binary).
    Material loaded;
    CHECK(loaded.load(path0));
    CHECK(std::string(loaded.getName()) == "material_0");
    CHECK(loaded.hasParameter("baseColorTexture"));

    fs::remove_all(outDir);
}

TEST_SUITE_END
