#pragma once
#include "IAYConverter.h"
#include "AYIntermediateAsset.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== AnimationConverter — 动画转换器 =====
// 将 AnimationData 转换为 .ayanm 二进制格式
class AnimationConverter : public IConverter {
public:
    AnimationConverter();
    explicit AnimationConverter(const std::string& sourcePath);
    virtual ~AnimationConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Animation"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

    // ===== 批量转换（从 IntermediateAsset） =====
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<AnimationData>& animations,
        const std::string& baseName
    );

private:
    std::string sourcePath;
    std::string outputDir;
    LoadOption loadOption = IConverter::LoadOption::Full;
    ayt::math::FGuid lastGuid;
};

} // namespace ayt::resource