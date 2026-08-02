#include "Converter\FontConverter.h"
#include "Loader\FontLoader.h"
#include "AYFontAsset.h"
#include <ayio/File.h>
#include <aystorage/Guid.h>
#include <cstring>

namespace ayt::resource
{

// ===== 工具函数 =====

static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

static std::string getBaseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    size_t dotPos = path.find_last_of('.');
    if (dotPos != std::string::npos && dotPos > pos) {
        return path.substr(pos + 1, dotPos - pos - 1);
    }
    return path.substr(pos + 1);
}

static std::string getExtension(const std::string& path) {
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) {
        return "";
    }
    return path.substr(dotPos + 1);
}

// ===== 默认字符集 =====
static const char* AY_DEFAULT_CHARSET =
    " !\"#$%&'()*+,-./0123456789:;<=>?"
    "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
    "`abcdefghijklmnopqrstuvwxyz{|}~";

// ===== FontConverter =====

FontConverter::FontConverter() = default;

FontConverter::FontConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void FontConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void FontConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

// ===== 简单的位图字体渲染 =====
// 注意：这是一个简化实现，实际生产环境应使用 FreeType 或类似库

struct BitmapGlyph {
    UInt32 width;
    UInt32 height;
    std::vector<UInt8> pixels;  // RGBA
    float advance;
};

static void generatePlaceholderAtlas(
    UInt32 atlasWidth, UInt32 atlasHeight,
    UInt32 fontSize,
    const std::string& charset,
    std::vector<UInt8>& atlasData,
    std::vector<Glyph>& glyphs,
    UInt32& outGlyphCount
) {
    atlasData.resize(atlasWidth * atlasHeight * 4, 0);
    glyphs.clear();

    UInt32 cellSize = fontSize + 2;
    UInt32 cols = atlasWidth / cellSize;
    UInt32 rows = atlasHeight / cellSize;

    UInt32 glyphIndex = 0;
    UInt32 x = cellSize / 2;
    UInt32 y = cellSize / 2;

    for (char c : charset) {
        UInt32 cp = static_cast<UInt8>(c);

        UInt32 glyphW = fontSize;
        UInt32 glyphH = fontSize;

        for (UInt32 py = 0; py < glyphH && (y + py) < atlasHeight; py++) {
            for (UInt32 px = 0; px < glyphW && (x + px) < atlasWidth; px++) {
                bool isSet = ((px + py) % 4 == 0);
                UInt32 atlasX = x + px;
                UInt32 atlasY = y + py;
                UInt32 idx = (atlasY * atlasWidth + atlasX) * 4;

                if (isSet) {
                    atlasData[idx + 0] = 255;
                    atlasData[idx + 1] = 255;
                    atlasData[idx + 2] = 255;
                    atlasData[idx + 3] = 255;
                }
            }
        }

        Glyph g;
        g.codepoint = cp;
        g.u = static_cast<float>(x) / static_cast<float>(atlasWidth);
        g.v = static_cast<float>(y) / static_cast<float>(atlasHeight);
        g.w = static_cast<float>(glyphW) / static_cast<float>(atlasWidth);
        g.h = static_cast<float>(glyphH) / static_cast<float>(atlasHeight);
        g.advance = static_cast<float>(glyphW) / static_cast<float>(atlasWidth);

        glyphs.push_back(g);
        glyphIndex++;

        x += cellSize;
        if (x + glyphW >= atlasWidth) {
            x = cellSize / 2;
            y += cellSize;
        }

        if (y + glyphH >= atlasHeight) {
            break;
        }
    }

    outGlyphCount = static_cast<UInt32>(glyphs.size());
}

ConversionResult FontConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        return result;
    }

    std::string ext = getExtension(sourcePath);
    for (auto& c : ext) c = static_cast<char>(tolower(c));

    if (ext != "ttf" && ext != "otf") {
        return result;
    }

    const char* charset = characterSet.empty() ? AY_DEFAULT_CHARSET : characterSet.c_str();

    FontAsset font;
    font.setName(getBaseName(sourcePath));
    font.setFontSize(fontSize);
    font.setAtlasSize(atlasWidth, atlasHeight);

    std::vector<UInt8> atlasData;
    std::vector<Glyph> glyphs;
    UInt32 glyphCount = 0;

    generatePlaceholderAtlas(atlasWidth, atlasHeight, fontSize,
                             charset, atlasData, glyphs, glyphCount);

    for (const auto& g : glyphs) {
        font.addGlyph(g);
    }

    font.setAtlasData(atlasData.data(), atlasData.size());

    std::vector<UInt8> binaryData;
    if (!font.saveToBinary(binaryData)) {
        return result;
    }

    std::string baseName = getBaseName(sourcePath);
    std::string outputFileName = baseName + ".ayfont";
    std::string virtualPath = "fonts/" + outputFileName;

    if (!outputDir.empty()) {
        std::string fullPath = outputDir + "/" + virtualPath;
        if (!ayt::io::File::exists(fullPath)) {
            if (!writeFile(fullPath, binaryData.data(), binaryData.size())) {
                return result;
            }
        }
    }

    ConversionResult::ConvertedResource res;
    res.path = virtualPath;
    res.type = "Font";
    res.size = static_cast<uint64_t>(binaryData.size());
    // F1.9: previously res.guid was left default-constructed; sidecar
    // + DB indexes then collided. Hash the saved binary data.
    res.guid = ayt::storage::Guid::computeFromData(binaryData.data(), binaryData.size());
    result.resources.push_back(res);

    return result;
}

std::vector<ConversionResult::ConvertedResource> FontConverter::convertAll(
    const std::vector<FontData>& fonts,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (size_t i = 0; i < fonts.size(); i++) {
        const auto& fontData = fonts[i];
        std::string name = fontData.name.empty()
            ? baseName + "_" + std::to_string(i) + ".ayfont"
            : fontData.name + ".ayfont";

        std::string virtualPath = "fonts/" + name;

        FontAsset font;
        font.setName(fontData.name.empty() ? name : fontData.name);
        font.setFontSize(fontData.fontSize > 0 ? fontData.fontSize : fontSize);
        font.setAtlasSize(atlasWidth, atlasHeight);

        for (const auto& g : fontData.glyphs) {
            Glyph glyph;
            glyph.codepoint = g.codepoint;
            glyph.u = g.u;
            glyph.v = g.v;
            glyph.w = g.w;
            glyph.h = g.h;
            glyph.advance = g.advance;
            font.addGlyph(glyph);
        }

        if (!fontData.atlasData.empty()) {
            font.setAtlasData(fontData.atlasData.data(), fontData.atlasData.size());
        }

        std::vector<UInt8> binaryData;
        if (!font.saveToBinary(binaryData)) {
            continue;
        }

        if (!outputDir.empty()) {
            std::string fullPath = outputDir + "/" + virtualPath;
            if (!ayt::io::File::exists(fullPath)) {
                writeFile(fullPath, binaryData.data(), binaryData.size());
            }
        }

        ConversionResult::ConvertedResource res;
        res.path = virtualPath;
        res.type = "Font";
        res.size = static_cast<uint64_t>(binaryData.size());
        // F1.9: hash the saved bytes; v0 left res.guid default.
        res.guid = ayt::storage::Guid::computeFromData(binaryData.data(), binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource