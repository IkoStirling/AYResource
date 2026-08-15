#include "AYResource/assetsImpl/Texture.h"
#include "AYResource/Loader/TextureLoader.h"
#include "AYResource/Converter/TextureConverter.h"
#include "AYResource/IConverter.h"
#include "AYTest.h"
#include <fstream>
#include <cstdio>
#include <cstring>

using namespace ayt::resource;

TEST_SUITE(TextureFormatTests)

    TEST_CASE(TextureConstants) {
        CHECK(ITexture::MAGIC == 0x48545841);  // 'AYTX'
        CHECK(ITexture::VERSION == 1);
    }

    TEST_CASE(TextureFormatValues) {
        CHECK(static_cast<UInt8>(TextureFormat::RGBA8) == 0);
        CHECK(static_cast<UInt8>(TextureFormat::RGB8) == 1);
        CHECK(static_cast<UInt8>(TextureFormat::BC1) == 2);
        CHECK(static_cast<UInt8>(TextureFormat::BC3) == 3);
        CHECK(static_cast<UInt8>(TextureFormat::BC4) == 4);
        CHECK(static_cast<UInt8>(TextureFormat::BC5) == 5);
        CHECK(static_cast<UInt8>(TextureFormat::BC7) == 6);
    }

TEST_SUITE_END

TEST_SUITE(TextureBinaryFormatTests)

    TEST_CASE(SaveAndLoadSolidColor) {
        Texture texture;
        texture.createSolidColor(4, 4, 255, 0, 0, 255);

        CHECK(texture.getWidth() == 4);
        CHECK(texture.getHeight() == 4);
        CHECK(texture.getFormat() == TextureFormat::RGBA8);
        CHECK(texture.getMipmapCount() == 1);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        CHECK(texture.saveToBinary(binaryData) == true);
        CHECK(binaryData.size() > 0);

        // 从二进制加载
        Texture loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);

        CHECK(loaded.getWidth() == 4);
        CHECK(loaded.getHeight() == 4);
        CHECK(loaded.getFormat() == TextureFormat::RGBA8);
        CHECK(loaded.getMipmapCount() == 1);

        // 验证像素数据
        const UInt8* mip0Data = loaded.getMipmapData(0);
        CHECK(mip0Data != nullptr);

        // 检查第一个像素 (应该是红色)
        CHECK(mip0Data[0] == 255);  // R
        CHECK(mip0Data[1] == 0);    // G
        CHECK(mip0Data[2] == 0);    // B
        CHECK(mip0Data[3] == 255);  // A
    }

       TEST_CASE(SaveAndLoadCheckerboard) {
        Texture texture;
        texture.createCheckerboard(8, 8, 4);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        CHECK(texture.saveToBinary(binaryData) == true);

        // 从二进制加载
        Texture loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);

        // 验证元数据
        CHECK(loaded.getWidth() == 8);
        CHECK(loaded.getHeight() == 8);
        CHECK(loaded.getFormat() == TextureFormat::RGBA8);
        CHECK(loaded.getMipmapCount() == 1);
        CHECK(loaded.getMipmapSize(0) == 8*8*4);

        // 验证图像数据
        const UInt8* origData = texture.getMipmapData(0);
        const UInt8* loadedData = loaded.getMipmapData(0);
        CHECK(loadedData != nullptr);

        for (size_t i = 0; i < texture.getMipmapSize(0); i++) {
            if (origData[i] != loadedData[i]) {
                printf("Mismatch at byte %zu: orig=%02x loaded=%02x\n",
                       i, origData[i], loadedData[i]);
                CHECK(false);
                break;
            }
        }
    }

    TEST_CASE(MipmapSizes) {
        Texture texture;
        texture.createSolidColor(16, 16, 128, 128, 128, 255);

        CHECK(texture.getMipmapCount() == 1);
        CHECK(texture.getMipmapSize(0) == 16 * 16 * 4);
    }

    TEST_CASE(InvalidData) {
        Texture texture;
        UInt8 invalidData[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(texture.loadFromBinary(invalidData, 8) == false);
    }

TEST_SUITE_END

TEST_SUITE(TextureConverterTests)

    TEST_CASE(CreateTextureConverter) {
        TextureConverter converter;
        CHECK(converter.isValid() == false);

        TextureConverter converter2("test.bmp");
        CHECK(converter2.isValid() == true);
    }

    TEST_CASE(TextureConverterGettersSetters) {
        TextureConverter converter;

        converter.setSourcePath("diffuse.png");
        CHECK(converter.getSourcePath() == "diffuse.png");

        converter.setOutputDir("output");
        CHECK(converter.getSourcePath() == "diffuse.png");  // Source path unchanged

        converter.setOutputFormat(TextureFormat::BC7);
        CHECK(converter.getOutputFormat() == TextureFormat::BC7);

        converter.setGenerateMipmaps(false);
        CHECK(converter.getGenerateMipmaps() == false);

        converter.setUsageSuffix("_n");
        CHECK(converter.getUsageSuffix() == "_n");
    }

    TEST_CASE(TextureConverterLoadOption) {
        TextureConverter converter;
        converter.setLoadOption(IConverter::LoadOption::Full);
        CHECK(converter.getLoadOption() == IConverter::LoadOption::Full);

        converter.setLoadOption(IConverter::LoadOption::MeshOnly);
        CHECK(converter.getLoadOption() == IConverter::LoadOption::MeshOnly);
    }

    TEST_CASE(ConverterFactory) {
        auto conv1 = IConverter::create("model.fbx");
        CHECK(conv1 != nullptr);
        CHECK(strcmp(conv1->getSourceType(), "FBX") == 0);

        auto conv2 = IConverter::create("model.gltf");
        CHECK(conv2 != nullptr);
        CHECK(strcmp(conv2->getSourceType(), "glTF") == 0);

        auto conv3 = IConverter::create("diffuse.png");
        CHECK(conv3 != nullptr);
        CHECK(strcmp(conv3->getSourceType(), "Texture") == 0);

        auto conv4 = IConverter::create("diffuse.bmp");
        CHECK(conv4 != nullptr);
        CHECK(strcmp(conv4->getSourceType(), "Texture") == 0);

        auto conv5 = IConverter::create("diffuse.tga");
        CHECK(conv5 != nullptr);
        CHECK(strcmp(conv5->getSourceType(), "Texture") == 0);

        auto conv6 = IConverter::create("unknown.xyz");
        CHECK(conv6 == nullptr);
    }

    TEST_CASE(ConvertInvalidPath) {
        TextureConverter converter("nonexistent.bmp");
        ConversionResult result = converter.convert();
        CHECK(result.resources.size() == 0);
    }

    TEST_CASE(ConvertEmptyPath) {
        TextureConverter converter;
        converter.setSourcePath("");
        ConversionResult result = converter.convert();
        CHECK(result.resources.size() == 0);
    }

TEST_SUITE_END

TEST_SUITE(TextureLoaderTests)

    TEST_CASE(CanLoadAytex) {
        TextureLoader loader;
        CHECK(loader.canLoad("texture.aytex") == true);
        CHECK(loader.canLoad("textures/diffuse_d.aytex") == true);
        CHECK(loader.canLoad("path/to/normal_n.aytex") == true);
        CHECK(loader.canLoad("texture.aymesh") == false);
        CHECK(loader.canLoad("texture.png") == false);
    }

    TEST_CASE(GetResourceType) {
        TextureLoader loader;
        CHECK(strcmp(loader.getResourceType(), "Texture") == 0);
    }

    TEST_CASE(LoadFromBinarySolidColor) {
        Texture original;
        original.createSolidColor(4, 4, 255, 128, 64, 255);

        std::vector<UInt8> binaryData;
        original.saveToBinary(binaryData);

        TextureLoader loader;
        auto resource = loader.loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(resource != nullptr);

        auto texture = std::dynamic_pointer_cast<Texture>(resource);
        CHECK(texture != nullptr);
        CHECK(texture->getWidth() == 4);
        CHECK(texture->getHeight() == 4);
        CHECK(texture->getFormat() == TextureFormat::RGBA8);
    }

    TEST_CASE(LoadFromBinaryCheckerboard) {
        Texture original;
        original.createCheckerboard(8, 8, 4);

        std::vector<UInt8> binaryData;
        original.saveToBinary(binaryData);

        TextureLoader loader;
        auto resource = loader.loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(resource != nullptr);

        auto texture = std::dynamic_pointer_cast<Texture>(resource);
        CHECK(texture != nullptr);
        CHECK(texture->getWidth() == 8);
        CHECK(texture->getHeight() == 8);
    }

    TEST_CASE(LoadNonExistentFile) {
        TextureLoader loader;
        auto resource = loader.load("nonexistent.aytex");
        CHECK(resource == nullptr);
    }

    TEST_CASE(LoadAsyncCallback) {
        Texture original;
        original.createSolidColor(2, 2, 255, 255, 255, 255);

        std::vector<UInt8> binaryData;
        original.saveToBinary(binaryData);

        TextureLoader loader;
        bool callbackInvoked = false;

        loader.loadAsync("test.aytex", [&callbackInvoked](std::shared_ptr<IResource>) {
            callbackInvoked = true;
        });

        CHECK(callbackInvoked == true);
    }

TEST_SUITE_END

TEST_SUITE(TextureIntegrationTests)

    TEST_CASE(CreateSaveLoadCycle) {
        // 创建棋盘格纹理
        Texture original;
        original.createCheckerboard(16, 16, 4);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        CHECK(original.saveToBinary(binaryData) == true);

        // 模拟文件加载流程
        const char* tempPath = "test_texture_integration.aytex";
        {
            std::ofstream file(tempPath, std::ios::binary);
            file.write(reinterpret_cast<const char*>(binaryData.data()), binaryData.size());
        }

        // 使用 loader 加载
        TextureLoader loader;
        auto resource = loader.load(tempPath);
        CHECK(resource != nullptr);

        auto loaded = std::dynamic_pointer_cast<Texture>(resource);
        CHECK(loaded != nullptr);
        CHECK(loaded->getWidth() == 16);
        CHECK(loaded->getHeight() == 16);
        CHECK(loaded->getFormat() == TextureFormat::RGBA8);
        CHECK(loaded->getMipmapCount() == 1);

        // 清理
        std::remove(tempPath);
    }

    TEST_CASE(ComputeMipSize) {
        // 测试 BC1 压缩大小计算
        UInt32 bc1Size = Texture::computeMipSize(16, 16, TextureFormat::BC1);
        // 16x16 = 4x4 blocks, each block = 8 bytes
        CHECK(bc1Size == 4 * 4 * 8);  // 128 bytes

        // 测试 RGBA8 大小计算
        UInt32 rgbaSize = Texture::computeMipSize(16, 16, TextureFormat::RGBA8);
        CHECK(rgbaSize == 16 * 16 * 4);  // 1024 bytes

        // 测试边界情况
        UInt32 tinySize = Texture::computeMipSize(1, 1, TextureFormat::RGBA8);
        CHECK(tinySize == 4);  // 1x1x4 bytes
    }

    TEST_CASE(TextureUsageSuffix) {
        TextureConverter converter;

        converter.setUsageSuffix("_d");  // diffuse
        CHECK(converter.getUsageSuffix() == "_d");

        converter.setUsageSuffix("_n");  // normal
        CHECK(converter.getUsageSuffix() == "_n");

        converter.setUsageSuffix("_s");  // specular
        CHECK(converter.getUsageSuffix() == "_s");
    }

TEST_SUITE_END