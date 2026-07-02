#pragma once
#include "IAYConverter.h"
#include "Converter\FBXParser.h"
#include "Converter\MeshConverter.h"
#include "Converter\MaterialConverter.h"
#include "Converter\TextureConverter.h"
#include "Converter\SkeletonConverter.h"
#include <memory>
#include <string>

namespace ayt::resource
{

// ===== FBXConverter — FBX →引擎格式 =====
// 使用 Parser + Converter 架构，支持一次性提取所有资源
class FBXConverter : public IConverter {
public:
    FBXConverter();
    explicit FBXConverter(const std::string& sourcePath);
    virtual ~FBXConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "FBX"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

    /// @brief 设置是否按 FBX 节点分离模型
    /// @param separate true=每个节点一个MeshData，false=合并所有到一个MeshData
    void setSeparateModels(bool separate) { separateModels = separate; }
    bool getSeparateModels() const { return separateModels; }

private:
    std::string sourcePath;
    std::string outputDir;
    IConverter::LoadOption loadOption = IConverter::LoadOption::Full;  // 默认 Full 模式
    bool separateModels = true;  // 默认分离，每个 aiMesh 一个 MeshData

    // 子转换器
    MeshConverter meshConverter;
    MaterialConverter materialConverter;
    TextureConverter textureConverter;
    SkeletonConverter skeletonConverter;
};

} // namespace ayt::resource