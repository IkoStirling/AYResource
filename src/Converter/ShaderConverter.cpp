#include "Converter/ShaderConverter.h"
#include "assetsImpl/AYShader.h"
#include <ayio/File.h>
#include <ayio/Path.h>
#include <aystorage/Guid.h>
#include <cstring>

namespace ayt::resource
{

// ===== ShaderConverter =====

ShaderConverter::ShaderConverter() = default;

ShaderConverter::ShaderConverter(const std::string& sourcePath)
    : _sourcePath(sourcePath) {
}

void ShaderConverter::setSourcePath(const std::string& path) {
    _sourcePath = path;
}

void ShaderConverter::setOutputDir(const std::string& dir) {
    _outputDir = dir;
}

std::string ShaderConverter::_detectShaderType(const std::string& source) const {
    // 简单的 GLSL/HLSL 检测
    if (source.find("#version") != std::string::npos ||
        source.find("void main()") != std::string::npos ||
        source.find("gl_Position") != std::string::npos) {
        return "glsl";
    }

    if (source.find("float4") != std::string::npos ||
        source.find("float3") != std::string::npos ||
        source.find("float2") != std::string::npos ||
        source.find("VS_OUTPUT") != std::string::npos ||
        source.find("PS_INPUT") != std::string::npos ||
        source.find("cbuffer") != std::string::npos) {
        return "hlsl";
    }

    return "glsl"; // 默认 GLSL
}

std::string ShaderConverter::_generateOutputPath(const std::string& name) const {
    if (!_outputDir.empty()) {
        std::string fullPath = _outputDir + "/" + name + ".ayshader";
        return fullPath;
    }
    return name + ".ayshader";
}

static bool writeFile(const std::string& path, const void* data, size_t size) {
    // F1.8: was raw BinaryWrite (crash leaves half-written file). Use
    // atomicWrite so a crash mid-write leaves the prior file intact.
    return ayt::io::File::atomicWrite(path, data, size);
}

ConversionResult ShaderConverter::convert() {
    ConversionResult result;

    if (_sourcePath.empty()) {
        return result;
    }

    // 读取源文件
    ayt::io::File file(_sourcePath, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return result;
    }

    size_t fileSize = file.size();
    std::vector<UInt8> sourceData(fileSize);
    if (file.read(sourceData.data(), fileSize) != fileSize) {
        return result;
    }

    // 创建 Shader 并填充数据
    Shader shader;
    shader.setSource(std::string(reinterpret_cast<const char*>(sourceData.data()), fileSize));

    // 从文件名提取名称
    size_t pos = _sourcePath.find_last_of("/\\");
    std::string fileName = (pos == std::string::npos) ? _sourcePath : _sourcePath.substr(pos + 1);
    size_t dotPos = fileName.find_last_of('.');
    if (dotPos != std::string::npos) {
        fileName = fileName.substr(0, dotPos);
    }
    shader.setName(fileName);

    // 设置入口点和 profile
    if (!_entryPoint.empty()) {
        shader.setEntryPoint(_entryPoint);
    } else {
        shader.setEntryPoint("main");
    }

    if (!_profile.empty()) {
        shader.setProfile(_profile);
    } else {
        // 根据源码类型自动检测
        std::string detectedType = _detectShaderType(shader.getSourceStr());
        if (detectedType == "hlsl") {
            shader.setProfile("hlsl"); // HLSL 默认配置
        } else {
            shader.setProfile("glsl_150"); // GLSL 默认配置
        }
    }

    // 生成二进制数据
    std::vector<UInt8> binaryData;
    if (!shader.saveToBinary(binaryData)) {
        return result;
    }

    // 生成输出路径
    std::string outputPath = _generateOutputPath(shader.getNameStr());

    // 写入文件（跳过已存在）
    if (!ayt::io::File::exists(outputPath)) {
        writeFile(outputPath, binaryData.data(), binaryData.size());
    }

    // 添加转换结果（含 GUID，便于 sidecar + cache 去重）
    ConversionResult::ConvertedResource res;
    res.path = outputPath;
    res.type = "Shader";
    res.size = static_cast<uint64_t>(binaryData.size());
    res.guid = ayt::storage::Guid::computeFromData(binaryData.data(), binaryData.size());
    result.resources.push_back(res);

    return result;
}

std::vector<ConversionResult::ConvertedResource> ShaderConverter::convertAll(
    const std::vector<ShaderData>& shaders,
    const std::string& baseName) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (const auto& shaderData : shaders) {
        Shader shader;
        shader.setName(shaderData.name.empty() ? baseName : shaderData.name);
        shader.setSource(shaderData.source);
        shader.setEntryPoint(shaderData.entryPoint.empty() ? "main" : shaderData.entryPoint);
        shader.setProfile(shaderData.profile.empty() ? "glsl_150" : shaderData.profile);

        std::vector<UInt8> binaryData;
        if (!shader.saveToBinary(binaryData)) {
            continue;
        }

        std::string outputPath = _generateOutputPath(shader.getNameStr());

        if (!ayt::io::File::exists(outputPath)) {
            writeFile(outputPath, binaryData.data(), binaryData.size());
        }

        ConversionResult::ConvertedResource res;
        res.path = outputPath;
        res.type = "Shader";
        res.size = static_cast<uint64_t>(binaryData.size());
        res.guid = ayt::storage::Guid::computeFromData(binaryData.data(), binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource