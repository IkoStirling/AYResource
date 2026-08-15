#pragma once
#include "AYResource/IConverter.h"
#include "AYResource/assetsImpl/Texture.h"
#include "AYResource/IntermediateAsset.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== TextureConverter — 纹理格式转换器 =====
// 将图片格式（PNG/BMP/TGA/DDS）转换为 .aytex 引擎格式
// 也支持从 TextureData 批量转换
class TextureConverter : public IConverter {
public:
    TextureConverter();
    explicit TextureConverter(const std::string& sourcePath);
    virtual ~TextureConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Texture"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

    // ===== 输出格式选项 =====
    void setOutputFormat(TextureFormat format) { outputFormat = format; }
    TextureFormat getOutputFormat() const { return outputFormat; }

    // ===== Mipmap 选项 =====
    void setGenerateMipmaps(bool generate) { generateMipmaps = generate; }
    bool getGenerateMipmaps() const { return generateMipmaps; }

    // ===== 直接拷贝选项 =====
    //开启后：跳过加载/解码/压缩，直接将源文件原始数据写入纹理路径
    // 适用于已压缩的纹理文件（如 DDS/BC7）直接流转
    void setPassthrough(bool enable) { passthrough = enable; }
    bool getPassthrough() const { return passthrough; }

    // ===== 用途后缀 =====
    void setUsageSuffix(const std::string& suffix) { usageSuffix = suffix; }
    const std::string& getUsageSuffix() const { return usageSuffix; }

    // ===== 从外部图片路径转换 =====
    // @param imagePath 源图片路径（PNG/BMP/TGA 等）
    // @param textureName 输出纹理名（不含扩展名）
    // @param baseFbxDir FBX 文件所在目录，用于解析相对路径
    ConversionResult convertFromPath(const std::string& imagePath,
                                   const std::string& textureName,
                                   const std::string& baseFbxDir);

    // ===== 批量转换（从 IntermediateAsset） =====
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<TextureData>& textures,
        const std::string& baseName
    );

private:
    std::string sourcePath;
    std::string outputDir;
    LoadOption loadOption = IConverter::LoadOption::MeshOnly;
    TextureFormat outputFormat = TextureFormat::BC7;  // 默认 BC7 压缩（离线高质量）
    bool generateMipmaps = true;
    bool passthrough = true;  // 直接拷贝模式（跳过加载/解码/压缩）
    std::string usageSuffix = "_d";  // 默认 diffuse
    ayt::math::FGuid lastGuid;
};

} // namespace ayt::resource