#pragma once
#include "AYResource/IConverter.h"
#include "AYResource/IntermediateAsset.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== SkeletonConverter — SkeletonData → .ayskel =====
class SkeletonConverter : public IConverter {
public:
    SkeletonConverter();
    explicit SkeletonConverter(const std::string& sourcePath);
    virtual ~SkeletonConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Skeleton"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

    // ===== 批量转换 =====
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<SkeletonData>& skeletons,
        const std::string& baseName
    );

private:
    std::string sourcePath;
    std::string outputDir;
    IConverter::LoadOption loadOption = IConverter::LoadOption::Full;
    ayt::math::FGuid lastGuid;
};

} // namespace ayt::resource
