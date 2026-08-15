#pragma once
#include "AYResource/IConverter.h"
#include "AYResource/assetsImpl/Material.h"
#include "AYResource/IntermediateAsset.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== MaterialConverter — 材质转换器 =====
// 将 JSON/YAML 材质描述转换为 .aymat 二进制格式
// 也支持从 MaterialData 批量转换
class MaterialConverter : public IConverter {
public:
    MaterialConverter();
    explicit MaterialConverter(const std::string& sourcePath);
    virtual ~MaterialConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Material"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

    // ===== 批量转换（从 IntermediateAsset） =====
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<MaterialData>& materials,
        const std::string& baseName
    );

private:
    std::string sourcePath;
    std::string outputDir;
    LoadOption loadOption = IConverter::LoadOption::Full;
    ayt::math::FGuid lastGuid;
};

} // namespace ayt::resource