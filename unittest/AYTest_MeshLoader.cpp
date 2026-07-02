#include "AYResource.h"
#include "Loader/MeshLoader.h"
#include "AYMesh.h"
#include "AYTest.h"
#include <vector>

using namespace ayt::resource;

TEST_SUITE(MeshLoaderTests)

    TEST_CASE(CreateCube) {
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(2.0f);

        CHECK(mesh->getVertexCount() == 24);
        CHECK(mesh->getIndexCount() == 36);
        CHECK(mesh->getSubmeshCount() == 1);
        CHECK(mesh->getMaterialSlotCount() == 1);
        CHECK(mesh->hasBounds() == true);

        FVector3 min, max;
        mesh->getBounds(min, max);
        CHECK(min.x == -1.0f);
        CHECK(max.x == 1.0f);
    }

    TEST_CASE(CreateSphere) {
        auto mesh = std::make_shared<Mesh>();
        mesh->createSphere(1.0f, 8);

        // 9x9 = 81 vertices, 8x8x6 = 384 indices
        CHECK(mesh->getVertexCount() == 81);
        CHECK(mesh->getIndexCount() == 384);
        CHECK(mesh->hasBounds() == true);
    }

    TEST_CASE(SaveAndLoadBinary_Cube) {
        // 创建立方体
        auto mesh = std::make_shared<Mesh>();
        mesh->createCube(1.0f);

        FVector3 min, max;
        mesh->getBounds(min, max);
        auto test = mesh->getAttributeInfo(MeshAttribute::Position).offset;

        UInt32 originalVertexCount = mesh->getVertexCount();
        UInt32 originalIndexCount = mesh->getIndexCount();

        // 保存到二进制
        std::vector<UInt8> binaryData;
        bool saveOk = mesh->saveToBinary(binaryData);
        CHECK(saveOk == true);
        CHECK(binaryData.empty() == false);

        // 从二进制加载
        auto loaded = std::make_shared<Mesh>();
        bool loadOk = loaded->loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(loadOk == true);

        // 验证数据一致性
        CHECK(loaded->getVertexCount() == originalVertexCount);
        CHECK(loaded->getIndexCount() == originalIndexCount);
        CHECK(loaded->getSubmeshCount() == 1);
        CHECK(loaded->getMaterialSlotCount() == 1);
        CHECK(loaded->hasBounds() == true);

        // 验证 bounds

        loaded->getBounds(min, max);
        CHECK(min.x == -0.5f);
        CHECK(max.x == 0.5f);
    }

    TEST_CASE(SaveAndLoadBinary_Sphere) {
        // 创建球体
        auto mesh = std::make_shared<Mesh>();
        mesh->createSphere(0.5f, 16);

        UInt32 originalVertexCount = mesh->getVertexCount();
        UInt32 originalIndexCount = mesh->getIndexCount();

        // 保存到二进制
        std::vector<UInt8> binaryData;
        mesh->saveToBinary(binaryData);

        // 从二进制加载
        auto loaded = std::make_shared<Mesh>();
        bool loadOk = loaded->loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(loadOk == true);

        CHECK(loaded->getVertexCount() == originalVertexCount);
        CHECK(loaded->getIndexCount() == originalIndexCount);
    }

    TEST_CASE(LoadFromFile) {
        // 这个测试需要已存在的 .aymesh 文件
        // 先跳过，真实文件需要通过 Converter 生成
        CHECK(true);
    }

TEST_SUITE_END

