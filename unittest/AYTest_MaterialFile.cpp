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
    matA->setFloat("metallic", 0.25f);

    auto matB = std::make_shared<Material>();
    matB->setName("MatB");
    matB->setShader("shaders/b.phoskia");
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
    CHECK(outA->getFloat("metallic") == 0.25f);
}

TEST_SUITE_END
