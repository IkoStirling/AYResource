#pragma once
#include "AYResource/IConverter.h"
#include "AYResource/assetsImpl/Shader.h"
#include "AYResource/IntermediateAsset.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== ShaderConverter — legacy offline converter =====
// Converts GLSL/HLSL source to deprecated `.ayshader` blobs.
// Runtime rendering uses Phoskia paths in `.aymat`; see docs/runtime-conventions.md.
class ShaderConverter : public IConverter {
public:
    ShaderConverter();
    explicit ShaderConverter(const std::string& sourcePath);
    virtual ~ShaderConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return _sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Shader"; }

    bool isValid() const override { return !_sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { _loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return _loadOption; }

    // ===== 着色器类型检测 =====
    void setAutoDetectType(bool autoDetect) { _autoDetectType = autoDetect; }
    bool getAutoDetectType() const { return _autoDetectType; }

    // ===== 输出格式选项 =====
    void setEntryPoint(const std::string& entry) { _entryPoint = entry; }
    const std::string& getEntryPoint() const { return _entryPoint; }

    void setProfile(const std::string& profile) { _profile = profile; }
    const std::string& getProfile() const { return _profile; }

    // ===== 批量转换（从 IntermediateAsset） =====
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<ShaderData>& shaders,
        const std::string& baseName
    );

private:
    std::string _detectShaderType(const std::string& source) const;
    std::string _generateOutputPath(const std::string& name) const;

    std::string _sourcePath;
    std::string _outputDir;
    std::string _entryPoint = "main";
    std::string _profile;
    LoadOption _loadOption = IConverter::LoadOption::Full;
    bool _autoDetectType = true;
};

} // namespace ayt::resource