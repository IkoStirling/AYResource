#pragma once
#include "AYResource/IConverter.h"
#include "AYResource/IntermediateAsset.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== FontConverter — 字体转换器 =====
// 将 TTF/OTF 字体文件转换为 .ayfont 渲染 atlas
// 从字体文件渲染字形到纹理图集
class FontConverter : public IConverter {
public:
    FontConverter();
    explicit FontConverter(const std::string& sourcePath);
    virtual ~FontConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Font"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

    // ===== 字体选项 =====
    void setFontSize(UInt32 size) { fontSize = size; }
    UInt32 getFontSize() const { return fontSize; }

    void setAtlasSize(UInt32 width, UInt32 height) {
        atlasWidth = width;
        atlasHeight = height;
    }

    // ===== 字符集选项 =====
    // 设置要渲染的字符集（默认为 Basic Latin + Latin Extended）
    void setCharacterSet(const std::string& charset) { characterSet = charset; }
    const std::string& getCharacterSet() const { return characterSet; }

    // ===== 批量转换 =====
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<FontData>& fonts,
        const std::string& baseName
    );

private:
    std::string sourcePath;
    std::string outputDir;
    LoadOption loadOption = IConverter::LoadOption::Full;
    UInt32 fontSize = 16;
    UInt32 atlasWidth = 512;
    UInt32 atlasHeight = 512;
    std::string characterSet;  // 默认空表示使用内置默认字符集
};

} // namespace ayt::resource