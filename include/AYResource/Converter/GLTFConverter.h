#pragma once
#include "AYResource/IConverter.h"
#include <memory>
#include <string>

namespace ayt::resource
{

// ===== GLTFConverter — glTF 格式转换器 =====
// 将 glTF 文件转换为引擎格式，输出文件 + 依赖清单
class GLTFConverter : public IConverter {
public:
    GLTFConverter();
    explicit GLTFConverter(const std::string& sourcePath);
    virtual ~GLTFConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "glTF"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

private:
    std::string sourcePath;
    std::string outputDir;
    IConverter::LoadOption loadOption = IConverter::LoadOption::MeshOnly;
};

} // namespace ayt::resource