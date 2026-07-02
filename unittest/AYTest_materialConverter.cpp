#include "AYResource.h"
#include "assetsImpl/AYMaterial.h"
#include "Converter/MaterialConverter.h"
#include "Loader/MaterialLoader.h"
#include "AYTest.h"
#include <vector>
#include <fstream>

using namespace ayt::resource;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

TEST_SUITE(MaterialConverterTests)

    TEST_CASE(CreateDefaultMaterial) {
        auto material = std::make_shared<Material>();
        material->createDefault();

        CHECK(std::string(material->getName()) == "Default");
        CHECK(std::string(material->getShader()) == "shaders/unlit.phoskia");
        CHECK(material->getParameterCount() == 4);
        CHECK(material->hasParameter("albedo") == true);
        CHECK(material->hasParameter("metallic") == true);
        CHECK(material->hasParameter("smoothness") == true);
        CHECK(material->hasParameter("emission") == true);

        float albedo[4];
        material->getFloat4("albedo", albedo);
        CHECK(albedo[0] == 1.0f);
        CHECK(albedo[1] == 1.0f);
        CHECK(albedo[2] == 1.0f);
        CHECK(albedo[3] == 1.0f);

        CHECK(material->getFloat("metallic") == 0.0f);
        CHECK(material->getFloat("smoothness") == 0.5f);
    }

    TEST_CASE(SaveAndLoadBinary_DefaultMaterial) {
        // 创建设定材质
        auto original = std::make_shared<Material>();
        original->createDefault();

        // 保存到二进制
        std::vector<UInt8> binaryData;
        bool saveOk = original->saveToBinary(binaryData);
        CHECK(saveOk == true);
        CHECK(binaryData.empty() == false);

        // 从二进制加载
        auto loaded = std::make_shared<Material>();
        bool loadOk = loaded->loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(loadOk == true);

        // 验证数据一致性
        CHECK(std::string(loaded->getName()) == "Default");
        CHECK(std::string(loaded->getShader()) == "shaders/unlit.phoskia");
        CHECK(loaded->getParameterCount() == 4);

        float albedo[4];
        loaded->getFloat4("albedo", albedo);
        CHECK(albedo[0] == 1.0f);
        CHECK(albedo[1] == 1.0f);
        CHECK(albedo[2] == 1.0f);
        CHECK(albedo[3] == 1.0f);

        CHECK(loaded->getFloat("metallic") == 0.0f);
        CHECK(loaded->getFloat("smoothness") == 0.5f);

        float emission[3];
        loaded->getFloat3("emission", emission);
        CHECK(emission[0] == 0.0f);
        CHECK(emission[1] == 0.0f);
        CHECK(emission[2] == 0.0f);
    }

    TEST_CASE(SetAndGetParameters) {
        auto material = std::make_shared<Material>();
        material->setName("TestMaterial");
        material->setShader("PBR");

        // Float
        material->setFloat("metallic", 0.8f);
        CHECK(material->getFloat("metallic") == 0.8f);

        // Float2
        float uvScale[2] = {2.0f, 3.0f};
        material->setFloat2("uvScale", uvScale);
        float outUVScale[2];
        material->getFloat2("uvScale", outUVScale);
        CHECK(outUVScale[0] == 2.0f);
        CHECK(outUVScale[1] == 3.0f);

        // Float3
        float emissive[3] = {1.0f, 0.5f, 0.2f};
        material->setFloat3("emissive", emissive);
        float outEmissive[3];
        material->getFloat3("emissive", outEmissive);
        CHECK(outEmissive[0] == 1.0f);
        CHECK(outEmissive[1] == 0.5f);
        CHECK(outEmissive[2] == 0.2f);

        // Float4
        float color[4] = {0.5f, 0.8f, 0.3f, 1.0f};
        material->setFloat4("albedo", color);
        float outColor[4];
        material->getFloat4("albedo", outColor);
        CHECK(outColor[0] == 0.5f);
        CHECK(outColor[1] == 0.8f);
        CHECK(outColor[2] == 0.3f);
        CHECK(outColor[3] == 1.0f);

        // Texture
        material->setTexture("mainTexture", "textures/wood.png");
        CHECK(std::string(material->getTexture("mainTexture")) == "textures/wood.png");

        // Int
        material->setInt("renderQueue", 2000);
        CHECK(material->getInt("renderQueue") == 2000);

        // Bool
        material->setBool("transparent", true);
        CHECK(material->getBool("transparent") == true);

        CHECK(material->getParameterCount() == 7);
    }

    TEST_CASE(SaveAndLoadBinary_WithTexture) {
        auto original = std::make_shared<Material>();
        original->setName("WoodPlank");
        original->setShader("Standard");

        float albedo[4] = {0.6f, 0.4f, 0.2f, 1.0f};
        original->setFloat4("albedo", albedo);
        original->setFloat("metallic", 0.0f);
        original->setFloat("smoothness", 0.7f);
        original->setTexture("mainTexture", "textures/wood_diffuse.png");
        original->setTexture("normalMap", "textures/wood_normal.png");

        // 保存到二进制
        std::vector<UInt8> binaryData;
        original->saveToBinary(binaryData);

        // 从二进制加载
        auto loaded = std::make_shared<Material>();
        bool loadOk = loaded->loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(loadOk == true);

        // 验证
        CHECK(std::string(loaded->getName()) == "WoodPlank");
        CHECK(std::string(loaded->getShader()) == "Standard");
        CHECK(std::string(loaded->getTexture("mainTexture")) == "textures/wood_diffuse.png");
        CHECK(std::string(loaded->getTexture("normalMap")) == "textures/wood_normal.png");

        float outAlbedo[4];
        loaded->getFloat4("albedo", outAlbedo);
        CHECK(outAlbedo[0] == 0.6f);
        CHECK(outAlbedo[1] == 0.4f);
        CHECK(outAlbedo[2] == 0.2f);
    }

    TEST_CASE(ConvertJsonToAymat) {
        // 创建测试 JSON 文件
        std::string testJsonPath = "D:/Projects/AYResource/test_output/test_material.json";
        std::string outputDir = "D:/Projects/AYResource/test_output";

        // 确保输出目录存在
        std::ifstream dirCheck(outputDir);
        if (!dirCheck.good()) {
            // 目录可能不存在，跳过此测试
            CHECK(true);
            return;
        }

        // 创建 JSON 测试文件
        std::ofstream jsonFile(testJsonPath);
        if (!jsonFile.is_open()) {
            CHECK(true);
            return;
        }

        jsonFile << R"({
            "name": "TestMaterial",
            "shader": "Standard",
            "parameters": {
                "albedo": [0.8, 0.2, 0.3, 1.0],
                "metallic": 0.5,
                "smoothness": 0.6,
                "emission": [0.1, 0.0, 0.0],
                "mainTexture": "textures/test.png"
            }
        })";
        jsonFile.close();

        // 使用转换器
        MaterialConverter converter(testJsonPath);
        CHECK(converter.isValid() == true);

        converter.setOutputDir(outputDir);

        ConversionResult result = converter.convert();
        CHECK(result.resources.size() > 0);
        CHECK(result.resources[0].type == std::string("Material"));

        // 验证输出文件
        std::string outPath = outputDir + "/" + result.resources[0].path;
        CHECK(fileExists(outPath) == true);

        // 加载验证
        auto loaded = std::make_shared<Material>();
        bool loadOk = loaded->load(outPath);
        CHECK(loadOk == true);

        CHECK(std::string(loaded->getName()) == "TestMaterial");
        CHECK(std::string(loaded->getShader()) == "Standard");

        float albedo[4];
        loaded->getFloat4("albedo", albedo);
        CHECK(albedo[0] == 0.8f);
        CHECK(albedo[1] == 0.2f);
        CHECK(albedo[2] == 0.3f);
        CHECK(albedo[3] == 1.0f);

        CHECK(loaded->getFloat("metallic") == 0.5f);
        CHECK(loaded->getFloat("smoothness") == 0.6f);

        float emission[3];
        loaded->getFloat3("emission", emission);
        CHECK(emission[0] == 0.1f);
        CHECK(emission[1] == 0.0f);
        CHECK(emission[2] == 0.0f);

        CHECK(std::string(loaded->getTexture("mainTexture")) == "textures/test.png");
    }

    TEST_CASE(MaterialLoader_CanLoad) {
        MaterialLoader loader;
        CHECK(loader.canLoad("materials/test.aymat") == true);
        CHECK(loader.canLoad("test.aymat") == true);
        CHECK(loader.canLoad("materials/test.aymesh") == false);
        CHECK(loader.canLoad("test.aymatabc") == false);
    }

    TEST_CASE(MaterialLoader_LoadFromBinary) {
        // 创建立方体网格对应的材质
        auto original = std::make_shared<Material>();
        original->setName("CubeMaterial");
        original->setShader("Standard");
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        original->setFloat4("albedo", color);
        original->setFloat("metallic", 0.0f);
        original->setFloat("smoothness", 0.5f);

        std::vector<UInt8> binaryData;
        original->saveToBinary(binaryData);

        // 使用 Loader 加载
        MaterialLoader loader;
        auto loaded = loader.loadFromBinary(binaryData.data(), binaryData.size());

        CHECK(loaded != nullptr);
        auto mat = std::dynamic_pointer_cast<Material>(loaded);
        CHECK(mat != nullptr);

        CHECK(std::string(mat->getName()) == "CubeMaterial");
        CHECK(mat->getFloat("metallic") == 0.0f);
    }

    TEST_CASE(InvalidMagicNumber) {
        // 创建无效 magic 的数据
        std::vector<UInt8> badData(100, 0);
        *reinterpret_cast<UInt32*>(badData.data()) = 0xDEADBEEF; // invalid magic

        auto material = std::make_shared<Material>();
        bool loadOk = material->loadFromBinary(badData.data(), badData.size());
        CHECK(loadOk == false);
    }

TEST_SUITE_END