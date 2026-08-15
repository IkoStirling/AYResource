#include "AYResource.h"
#include "AYResource/Converter/FBXConverter.h"
#include "AYResource/Loader/MeshLoader.h"
#include "AYResource/Loader/MaterialLoader.h"
#include "AYResource/Loader/TextureLoader.h"
#include "AYResource/Loader/SkeletonLoader.h"
#include "AYTest.h"
#include "AYResource/assetsImpl/Mesh.h"
#include "AYResource/assetsImpl/Material.h"
#include "AYResource/assetsImpl/Texture.h"
#include "AYResource/assetsImpl/Skeleton.h"
#include <vector>
#include <fstream>
#include <cstring>

using namespace ayt::resource;
using namespace ayt::math;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

static std::string fbxTestOutputDir() {
    return ayt::test::testTmpPath("test_output");
}

static std::string fbxTestOutputStaticDir() {
    return ayt::test::testTmpPath("test_output_static");
}

TEST_SUITE(FBXConverterTests)

// 注释掉 — 该 case 依赖磁盘上旧版 (Submesh 12 字节) .aymesh 文件
// 旧文件 SUBM chunk size = N × 12,新 loader 按 N × 16 读会越界。
// 应当先跑 FBXConverter 生成新版 .aymesh 再读,不在 unit test 里做。
/*
TEST_CASE(FileValide) {
    std::string fullPath = "D:/Projects/AYRuntime/AYResource/test_output/meshes/Sour_RootNode_Sour_mesh.aymesh";
    CHECK(fileExists(fullPath) == true);
    ...
}
*/

    // Suzanne 路径不在主测试机器上,默认 skip
    TEST_CASE(ConvertSuzanne) {
        std::string fbxPath = "D:/Projects/suzanne.fbx";
        if (!fileExists(fbxPath)) {
            printf("    [SKIP] %s not found\n", fbxPath.c_str());
            return;
        }

        FBXConverter converter(fbxPath);
        CHECK(converter.isValid() == true);

        // 设置输出目录
        converter.setOutputDir(fbxTestOutputDir());

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);

        // 验证输出文件存在
        std::string outPath = fbxTestOutputDir() + "/" + result.resources[0].path;
        CHECK(fileExists(outPath) == true);

        // 验证资源信息
        CHECK(result.resources[0].type == "Mesh");
        CHECK(result.resources[0].size > 0);
    }

    TEST_CASE(ConvertCube) {
        std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/cube.fbx";
        if (!fileExists(fbxPath)) {
            printf("    [SKIP] %s not found\n", fbxPath.c_str());
            return;
        }

        FBXConverter converter(fbxPath);
        CHECK(converter.isValid() == true);

        converter.setOutputDir(fbxTestOutputDir());

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);

        // 验证输出文件
        std::string outPath = fbxTestOutputDir() + "/" + result.resources[0].path;
        CHECK(fileExists(outPath) == true);
    }

    // Sour Miku FBX 解析很慢,在 CI / 频繁跑测试时建议注释
    /*
    TEST_CASE(ConvertSourMikuFull) {

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);

        // 统计各类型资源数量
        size_t meshCount = 0;
        size_t materialCount = 0;
        size_t textureCount = 0;
        size_t skeletonCount = 0;
        for (const auto& res : result.resources) {
            if (res.type == "Mesh") meshCount++;
            else if (res.type == "Material") materialCount++;
            else if (res.type == "Texture") textureCount++;
            else if (res.type == "Skeleton") skeletonCount++;
        }

        printf("=== Conversion Result ===\n");
        printf("  total resources: %zu\n", result.resources.size());
        printf("  Mesh: %zu, Material: %zu, Texture: %zu, Skeleton: %zu\n", meshCount, materialCount, textureCount, skeletonCount);
        printf("  dependencies: %zu\n", result.dependencies.size());

        // 验证有 Mesh 资源
        CHECK(meshCount > 0);
        printf("  [OK] Mesh count: %zu\n", meshCount);

        // ===== 验证 Mesh =====
        for (size_t i = 0; i < result.resources.size(); i++) {
            const auto& res = result.resources[i];
            if (res.type != "Mesh") continue;

            printf("  Testing Mesh[%zu]: path=%s size=%lld\n", i, res.path.c_str(), (long long)res.size);

            // 验证文件存在
            std::string fullPath = fbxTestOutputDir() + "/" + res.path;
            CHECK(fileExists(fullPath) == true);
            printf("    [OK] File exists\n");

            // 读取二进制
            std::ifstream rawFile(fullPath, std::ios::binary);
            CHECK(rawFile.is_open() == true);
            rawFile.seekg(0, std::ios::end);
            size_t rawSize = rawFile.tellg();
            rawFile.seekg(0, std::ios::beg);

            std::vector<UInt8> fileData(rawSize);
            rawFile.read(reinterpret_cast<char*>(fileData.data()), rawSize);

            // 验证 header
            UInt32 magic = *reinterpret_cast<UInt32*>(fileData.data());
            UInt16 version = *reinterpret_cast<UInt16*>(fileData.data() + 4);
            CHECK(magic == 0x484D5941);  // 'AYMH'
            CHECK(version == 1);
            printf("    [OK] Magic=0x%08X, Version=%d\n", magic, version);

            // 加载并验证
            MeshLoader loader;
            auto mesh = std::dynamic_pointer_cast<Mesh>(
                loader.loadFromBinary(fileData.data(), fileData.size()));
            CHECK(mesh != nullptr);
            CHECK(mesh->getVertexCount() > 0);
            CHECK(mesh->getIndexCount() > 0);
            printf("    [OK] vertexCount=%u, indexCount=%u\n",
                   mesh->getVertexCount(), mesh->getIndexCount());

            // 验证 bounds
            CHECK(mesh->hasBounds() == true);
            FVector3 min, max;
            mesh->getBounds(min, max);
            CHECK(min.x <= max.x && min.y <= max.y && min.z <= max.z);
            printf("    [OK] Bounds valid\n");

            // 验证顶点数据
            CHECK(mesh->getVertexData() != nullptr);
            CHECK(mesh->getIndexData() != nullptr);

            // 验证 submesh 和 material slot
            printf("    [OK] submeshCount=%u, materialSlotCount=%u\n",
                   mesh->getSubmeshCount(), mesh->getMaterialSlotCount());
        }

        // ===== 验证 Material =====
        printf("  Testing Materials...\n");
        for (size_t i = 0; i < result.resources.size(); i++) {
            const auto& res = result.resources[i];
            if (res.type != "Material") continue;

            std::string fullPath = fbxTestOutputDir() + "/" + res.path;
            CHECK(fileExists(fullPath) == true);
            printf("    Material: %s (size=%lld) [OK]\n", res.path.c_str(), (long long)res.size);
        }
        CHECK(materialCount > 0);
        printf("  [OK] Material count: %zu\n", materialCount);

        // ===== 验证 Texture（如果有） =====
        if (textureCount > 0) {
            printf("  Testing Textures...\n");
            for (size_t i = 0; i < result.resources.size(); i++) {
                const auto& res = result.resources[i];
                if (res.type != "Texture") continue;

                std::string fullPath = fbxTestOutputDir() + "/" + res.path;
                CHECK(fileExists(fullPath) == true);
                printf("    Texture: %s (size=%lld) [OK]\n", res.path.c_str(), (long long)res.size);
            }
            printf("  [OK] Texture count: %zu\n", textureCount);
        } else {
            printf("  [INFO] No embedded textures in this FBX\n");
        }

        // ===== 验证 Skeleton（如果有） =====
        if (skeletonCount > 0) {
            printf("  Testing Skeletons...\n");
            for (size_t i = 0; i < result.resources.size(); i++) {
                const auto& res = result.resources[i];
                if (res.type != "Skeleton") continue;

                printf("  Testing Skeleton[%zu]: path=%s size=%lld\n", i, res.path.c_str(), (long long)res.size);

                std::string fullPath = fbxTestOutputDir() + "/" + res.path;
                CHECK(fileExists(fullPath) == true);
                printf("    [OK] File exists\n");

                // 读取二进制
                std::ifstream rawFile(fullPath, std::ios::binary);
                CHECK(rawFile.is_open() == true);
                rawFile.seekg(0, std::ios::end);
                size_t rawSize = rawFile.tellg();
                rawFile.seekg(0, std::ios::beg);

                std::vector<UInt8> fileData(rawSize);
                rawFile.read(reinterpret_cast<char*>(fileData.data()), rawSize);

                // 验证 header
                UInt32 magic = *reinterpret_cast<UInt32*>(fileData.data());
                CHECK(magic == 0x534B4C4E);  // 'SKLN'
                printf("    [OK] Magic=0x%08X\n", magic);

                // 加载并验证
                SkeletonLoader loader;
                auto skeleton = std::dynamic_pointer_cast<Skeleton>(
                    loader.loadFromBinary(fileData.data(), fileData.size()));
                CHECK(skeleton != nullptr);
                CHECK(skeleton->getBoneCount() > 0);
                printf("    [OK] boneCount=%zu\n", skeleton->getBoneCount());
            }
            printf("  [OK] Skeleton count: %zu\n", skeletonCount);
        } else {
            printf("  [INFO] No skeletons in this FBX\n");
        }

        // ===== 验证依赖关系 =====
        printf("  Testing Dependencies...\n");
        CHECK(result.dependencies.size() > 0);
        int dep_count = 0;
        for (const auto& dep : result.dependencies) {
			if (dep.from.empty() || dep.to.empty() ){
				dep_count++;
			}

        }
        CHECK(dep_count == 0);
    }
    */

    //TEST_CASE(ConvertSourMikuMergedVsSeparate) {
    //    // 注意：SourMiku 只有 1 个顶级节点，所以无论哪种模式都只输出 1 个 .aymesh
    //    // 分离模式只在多顶级节点的 FBX 下才有意义
    //    std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx";
    //    // 验证两种模式都能正常工作
    //    for (bool separate : {false, true}) {
    //        FBXConverter converter(fbxPath);
    //        converter.setLoadOption(IConverter::LoadOption::Full);
    //        converter.setSeparateModels(separate);
    //        converter.setOutputDir(fbxTestOutputDir() + "/" + std::string(separate ? "separate" : "merged"));
    //        ConversionResult result = converter.convert();
    //        size_t meshCount = 0;
    //        for (const auto& res : result.resources) {
    //            if (res.type == "Mesh") meshCount++;
    //        }
    //        printf("=== Mode: %s, Mesh count: %zu ===\n", separate ? "Separate" : "Merged", meshCount);
    //        CHECK(meshCount > 0);
    //    }
    //}

    TEST_CASE(FactoryCreate) {
        // 测试工厂方法
        auto fbxConv = IConverter::create("test.fbx");
        CHECK(fbxConv != nullptr);
        CHECK(strcmp(fbxConv->getSourceType(), "FBX") == 0);

        auto gltfConv = IConverter::create("test.gltf");
        CHECK(gltfConv != nullptr);
        CHECK(strcmp(gltfConv->getSourceType(), "glTF") == 0);

        auto glbConv = IConverter::create("test.glb");
        CHECK(glbConv != nullptr);
        CHECK(strcmp(glbConv->getSourceType(), "glTF") == 0);

        auto unknownConv = IConverter::create("test.obj");
        CHECK(unknownConv == nullptr);
    }

    // ===== R-02 新增测试 =====

    // Sour Miku FBX 解析很慢 — 注释掉,等性能优化后再启用
    // 验证 FBX 含动画时,resources 里出现 type == "Animation" 且 .ayanm 文件存在并以 'AYNM' 开头
    /*
    TEST_CASE(AnimationsAreEmitted) {
        std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx";
        FBXConverter converter(fbxPath);
        converter.setLoadOption(IConverter::LoadOption::Full);
        converter.setSeparateModels(true);
        converter.setOutputDir(fbxTestOutputDir());

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);

        size_t animCount = 0;
        for (const auto& res : result.resources) {
            if (res.type != "Animation") continue;
            ++animCount;

            // 文件存在
            std::string fullPath = fbxTestOutputDir() + "/" + res.path;
            CHECK(fileExists(fullPath) == true);

            // magic == 'AYNM' (0x4E4D5941 little-endian)
            std::ifstream rawFile(fullPath, std::ios::binary);
            rawFile.seekg(0, std::ios::end);
            size_t rawSize = (size_t)rawFile.tellg();
            rawFile.seekg(0, std::ios::beg);
            std::vector<UInt8> fileData(rawSize);
            rawFile.read(reinterpret_cast<char*>(fileData.data()), rawSize);
            CHECK(fileData.size() >= 8);
            UInt32 magic = *reinterpret_cast<UInt32*>(fileData.data());
            CHECK(magic == 0x4E4D5941);
        }
        printf("    [INFO] Sour.fbx emitted %zu animations\n", animCount);
    }
    */

    // Sour Miku FBX 解析很慢 — 注释掉
    // 验证 .aydep.json 包含 mesh→skeleton 与 skeleton→animation 边
    /*
    TEST_CASE(DependenciesContainMeshToSkeleton) {
        std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx";
        FBXConverter converter(fbxPath);
        converter.setLoadOption(IConverter::LoadOption::Full);
        converter.setSeparateModels(true);
        converter.setOutputDir(fbxTestOutputDir());

        ConversionResult result = converter.convert();

        // 必须有 skeleton 才会写 mesh→skel 边
        bool hasSkeleton = false;
        bool hasAnim = false;
        for (const auto& res : result.resources) {
            if (res.type == "Skeleton") hasSkeleton = true;
            if (res.type == "Animation") hasAnim = true;
        }
        if (!hasSkeleton) {
            printf("    [SKIP] Sour.fbx has no skeleton\n");
            return;
        }

        bool sawMeshToSkel = false;
        bool sawSkelToAnim = false;
        for (const auto& dep : result.dependencies) {
            if (dep.from.find("meshes/") == 0 && dep.to.find("skeletons/") == 0) {
                sawMeshToSkel = true;
            }
            if (dep.from.find("skeletons/") == 0 && dep.to.find("animations/") == 0) {
                sawSkelToAnim = true;
            }
        }
        CHECK(sawMeshToSkel);
        if (hasAnim) {
            CHECK(sawSkelToAnim);
        }
    }
    */

    // 静态 FBX (cube.fbx 没有 anim/skel) 必须不产生 animation
    TEST_CASE(StaticFbxHasNoAnimations) {
        std::string fbxPath = "D:/Projects/AliyatRenderer/assets/core/models/cube.fbx";
        if (!fileExists(fbxPath)) {
            printf("    [SKIP] %s not found\n", fbxPath.c_str());
            return;
        }
        FBXConverter converter(fbxPath);
        converter.setLoadOption(IConverter::LoadOption::Full);
        converter.setSeparateModels(true);
        converter.setOutputDir(fbxTestOutputStaticDir());

        ConversionResult result = converter.convert();
        for (const auto& res : result.resources) {
            CHECK(res.type != "Animation");
        }
    }

TEST_SUITE_END
