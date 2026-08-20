#include "AYResource.h"
#include "AYResource/Loader/MaterialFile.h"
#include "AYTest.h"

#include <vector>

using namespace ayt::resource;

TEST_SUITE(MaterialFileTests)

TEST_CASE(save_and_load_multi_material_file)
{
    auto matA = std::make_shared<Material>();
    matA->setName("MatA");
    matA->setShader("shaders/a.phoskia");
    matA->setSurfaceProperties(MaterialAlphaMode::Mask, 0.37f, true);
    matA->setFloat("metallic", 0.25f);

    auto matB = std::make_shared<Material>();
    matB->setName("MatB");
    matB->setShader("shaders/b.phoskia");
    matB->setSurfaceProperties(MaterialAlphaMode::Blend, 0.5f, false);
    const float colorB[] = {1.0f, 0.5f, 0.25f, 1.0f};
    matB->setFloat4("baseColor", colorB);

    MaterialFile file;
    file.addMaterial(matA);
    file.addMaterial(matB);

    std::vector<UInt8> bytes;
    CHECK(file.saveToBinary(bytes));
    CHECK(bytes.size() > 0);

    MaterialFile loaded;
    CHECK(loaded.loadFromBinary(bytes.data(), bytes.size()));
    CHECK(loaded.getMaterialCount() == 2);

    auto outA = loaded.getMaterial(0);
    auto outB = loaded.getMaterial(1);
    CHECK(outA != nullptr);
    CHECK(outB != nullptr);
    CHECK(outA->getName() == std::string("MatA"));
    CHECK(outB->getName() == std::string("MatB"));
    CHECK(outA->getShader() == std::string("shaders/a.phoskia"));
    CHECK(outB->getShader() == std::string("shaders/b.phoskia"));
    CHECK(outA->getAlphaMode() == MaterialAlphaMode::Mask);
    CHECK(outA->getAlphaCutoff() == 0.37f);
    CHECK(outA->isDoubleSided());
    CHECK(outB->getAlphaMode() == MaterialAlphaMode::Blend);
    CHECK_FALSE(outB->isDoubleSided());
    CHECK(outA->getFloat("metallic") == 0.25f);
}

TEST_CASE(save_and_load_single_material_surface_contract)
{
    Material original;
    original.setName("HairCards");
    original.setShader("shaders/pbr.phoskia");
    original.setSurfaceProperties(MaterialAlphaMode::Mask, 0.42f, true);
    original.setTexture("baseColorTexture", "textures/hair.png");

    std::vector<UInt8> bytes;
    CHECK(original.saveToBinary(bytes));

    Material loaded;
    CHECK(loaded.loadFromBinary(bytes.data(), bytes.size()));
    CHECK(loaded.getAlphaMode() == MaterialAlphaMode::Mask);
    CHECK(loaded.getAlphaCutoff() == 0.42f);
    CHECK(loaded.isDoubleSided());
    CHECK(loaded.getTexture("baseColorTexture") ==
          std::string("textures/hair.png"));
}

TEST_SUITE_END
