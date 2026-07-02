#include "AYResource.h"
#include "interface/assetsDefs/IAYFontAsset.h"
#include "assetsImpl/AYFontAsset.h"
#include "Loader/FontLoader.h"
#include "AYTest.h"
#include <fstream>
#include <cstdio>

using namespace ayt::resource;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

TEST_SUITE(FontLoaderTests)

    TEST_CASE(CreateFontAsset) {
        FontAsset font;
        font.setName("TestFont");
        font.setFontSize(16);
        font.setAtlasSize(256, 256);

        CHECK(strcmp(font.getName(), "TestFont") == 0);
        CHECK(font.getFontSize() == 16);
        CHECK(font.getAtlasWidth() == 256);
        CHECK(font.getAtlasHeight() == 256);
    }

    TEST_CASE(AddGlyphs) {
        FontAsset font;
        font.setName("TestFont");
        font.setFontSize(16);
        font.setAtlasSize(256, 256);

        // 添加字形
        Glyph glyph;
        glyph.codepoint = 'A';
        glyph.u = 0.0f;
        glyph.v = 0.0f;
        glyph.w = 0.1f;
        glyph.h = 0.1f;
        glyph.advance = 0.05f;
        font.addGlyph(glyph);

        CHECK(font.getGlyphCount() == 1);

        // 查询字形
        float u, v, w, h, advance;
        CHECK(font.getGlyphInfo('A', u, v, w, h, advance) == true);
        CHECK(u == 0.0f);
        CHECK(v == 0.0f);
        CHECK(w == 0.1f);
        CHECK(h == 0.1f);
        CHECK(advance == 0.05f);

        // 不存在的字形
        CHECK(font.getGlyphInfo('Z', u, v, w, h, advance) == false);
    }

    TEST_CASE(SetAtlasData) {
        FontAsset font;
        font.setAtlasSize(64, 64);

        std::vector<UInt8> atlasData(64 * 64 * 4, 128);  // RGBA
        font.setAtlasData(atlasData.data(), atlasData.size());

        const UInt8* data = font.getAtlasData();
        CHECK(data != nullptr);
        CHECK(data[0] == 128);
    }

    TEST_CASE(SaveAndLoadBinary) {
        FontAsset original;
        original.setName("TestFont");
        original.setFontSize(16);
        original.setAtlasSize(128, 128);

        // 添加几个字形
        Glyph glyph;
        glyph.codepoint = 'A';
        glyph.u = 0.0f; glyph.v = 0.0f;
        glyph.w = 0.1f; glyph.h = 0.1f;
        glyph.advance = 0.05f;
        original.addGlyph(glyph);

        glyph.codepoint = 'B';
        glyph.u = 0.1f;
        original.addGlyph(glyph);

        std::vector<UInt8> atlasData(128 * 128 * 4, 255);
        original.setAtlasData(atlasData.data(), atlasData.size());

        // 保存到二进制
        std::vector<UInt8> binaryData;
        CHECK(original.saveToBinary(binaryData) == true);
        CHECK(binaryData.empty() == false);

        // 从二进制加载
        FontAsset loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);

        CHECK(strcmp(loaded.getName(), original.getName()) == 0);
        CHECK(loaded.getFontSize() == original.getFontSize());
        CHECK(loaded.getGlyphCount() == original.getGlyphCount());
        CHECK(loaded.getAtlasWidth() == original.getAtlasWidth());
        CHECK(loaded.getAtlasHeight() == original.getAtlasHeight());
    }

    TEST_CASE(LoadNonExistentFile) {
        FontAsset font;
        CHECK(font.load("nonexistent.ayfont") == false);
    }

    TEST_CASE(CanLoad) {
        FontLoader loader;
        CHECK(loader.canLoad("font.ayfont") == true);
        CHECK(loader.canLoad("path/to/text.ayfont") == true);
        CHECK(loader.canLoad("test.aymesh") == false);
        CHECK(loader.canLoad("test.ayfontabc") == false);
    }

    TEST_CASE(GetResourceType) {
        FontLoader loader;
        CHECK(strcmp(loader.getResourceType(), "Font") == 0);
    }

TEST_SUITE_END
