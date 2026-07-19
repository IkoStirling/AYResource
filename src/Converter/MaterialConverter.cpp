#include "Converter\MaterialConverter.h"
#include "Loader\MaterialFile.h"
#include "ayio/File.h"
#include <AYSerializer.h>
#include <aystorage/Guid.h>

namespace ayt::resource
{

MaterialConverter::MaterialConverter() = default;

MaterialConverter::MaterialConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void MaterialConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void MaterialConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

static std::string getFileName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

static std::string replaceExt(const std::string& path, const std::string& newExt) {
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return path + newExt;
    }
    return path.substr(0, pos) + newExt;
}

// Helper: try to read a float array [x, y, ...] - returns false if field doesn't exist
static bool tryReadFloatArray(ayt::serializer::ISerializer& s, const char* name, Float32* out, int count) {
    s.beginArray(name);
    for (int i = 0; i < count; i++) {
        s.field(nullptr, out[i]);
    }
    s.endArray();
    return true;
}

ConversionResult MaterialConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        return result;
    }

    // 使用 AYSerializer 创建 JSON 序列化器读取文件
    auto serializer = ayt::serializer::createSerializer(ayt::serializer::Format::Json);
    if (!serializer || !serializer->loadFromFile(sourcePath)) {
        return result;
    }

    // 读取根对象
    serializer->beginObject(nullptr);

    // 读取 name 和 shader
    std::string name, shader;
    serializer->field("name", name);
    serializer->field("shader", shader);
    if (shader.empty()) {
        shader = "shaders/pbr.phoskia";
    }

    // 创建材质
    auto material = std::make_shared<Material>();
    material->clear();
    material->setName(name);
    material->setShader(shader);

    // 读取 parameters 对象
    serializer->beginObject("parameters");

    Float32 fvals[4] = {0};

    // albedo/baseColor - float4
    tryReadFloatArray(*serializer, "albedo", fvals, 4);
    material->setFloat4("albedo", fvals);
    tryReadFloatArray(*serializer, "baseColor", fvals, 4);
    material->setFloat4("baseColor", fvals);

    // metallic, roughness, smoothness, opacity - float
    Float32 fsingle = 0.0f;
    tryReadFloatArray(*serializer, "metallic", &fsingle, 1);
    material->setFloat("metallic", fsingle);
    tryReadFloatArray(*serializer, "roughness", &fsingle, 1);
    material->setFloat("roughness", fsingle);
    tryReadFloatArray(*serializer, "smoothness", &fsingle, 1);
    material->setFloat("smoothness", fsingle);
    tryReadFloatArray(*serializer, "opacity", &fsingle, 1);
    material->setFloat("opacity", fsingle);

    // emission - float3
    tryReadFloatArray(*serializer, "emission", fvals, 3);
    material->setFloat3("emission", fvals);

    // normal - float
    tryReadFloatArray(*serializer, "normal", &fsingle, 1);
    material->setFloat("normal", fsingle);

    // mainTexture - string
    std::string texPath;
    serializer->field("mainTexture", texPath);
    if (!texPath.empty()) {
        material->setTexture("mainTexture", texPath.c_str());
    }

    serializer->endObject(); // parameters
    serializer->endObject(); // root

    material->_loaded = true;

    // 计算 GUID（基于 name + shader + 参数内容）
    std::vector<UInt8> contentData;
    contentData.insert(contentData.end(), name.begin(), name.end());
    contentData.insert(contentData.end(), shader.begin(), shader.end());
    // 添加参数数据
    for (const auto& param : material->_params) {
        UInt32 nameLen = static_cast<UInt32>(param.first.size());
        contentData.resize(contentData.size() + sizeof(UInt32) + nameLen + sizeof(UInt8));
        UInt8* ptr = contentData.data() + contentData.size() - sizeof(UInt32) - nameLen - sizeof(UInt8);
        *reinterpret_cast<UInt32*>(ptr) = nameLen; ptr += sizeof(UInt32);
        memcpy(ptr, param.first.data(), nameLen); ptr += nameLen;
        *reinterpret_cast<UInt8*>(ptr) = static_cast<UInt8>(param.second.type);
    }
    lastGuid = ayt::storage::Guid::computeFromData(contentData.data(), contentData.size());
    material->setGuid(lastGuid);

    // 保存到二进制
    std::vector<UInt8> binaryData;
    if (!material->saveToBinary(binaryData)) {
        return result;
    }

    // 生成输出文件名
    std::string sourceFileName = getFileName(sourcePath);
    std::string outputFileName = replaceExt(sourceFileName, ".aymat");
    std::string virtualPath = "materials/" + outputFileName;

    // 写入输出目录
    if (!outputDir.empty()) {
        std::string fullPath = outputDir + "/" + virtualPath;
        if (!ayt::io::File::exists(fullPath)) {
            writeFile(fullPath, binaryData.data(), binaryData.size());
        }
    }

    // 构建资源信息
    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Material";
    res.size = static_cast<int64_t>(binaryData.size());
    result.resources.push_back(res);

    return result;
}

std::vector<ConversionResult::ConvertedResource> MaterialConverter::convertAll(
    const std::vector<MaterialData>& materials,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    if (materials.empty()) {
        return results;
    }

    // 创建 MaterialFile 并添加所有材质
    auto matFile = std::make_shared<MaterialFile>();

    for (size_t i = 0; i < materials.size(); i++) {
        const auto& matData = materials[i];

        // 创建 Material 并填充数据
        auto material = std::make_shared<Material>();
        material->clear();
        material->setName(matData.name.empty() ? baseName : matData.name.c_str());
        material->setShader(matData.shader.c_str());

        for (const auto& param : matData.parameters) {
            switch (param.type) {
            case MaterialParamType::Float:
                material->setFloat(param.name.c_str(), param.floatValue);
                break;
            case MaterialParamType::Float2:
                material->setFloat2(param.name.c_str(), param.float2Value);
                break;
            case MaterialParamType::Float3:
                material->setFloat3(param.name.c_str(), param.float3Value);
                break;
            case MaterialParamType::Float4:
                material->setFloat4(param.name.c_str(), param.float4Value);
                break;
            case MaterialParamType::Texture2D:
                material->setTexture(param.name.c_str(), param.texturePath.c_str());
                break;
            case MaterialParamType::Int:
                material->setInt(param.name.c_str(), param.intValue);
                break;
            case MaterialParamType::Bool:
                material->setBool(param.name.c_str(), param.boolValue);
                break;
            default:
                break;
            }
        }

        material->_loaded = true;
        matFile->addMaterial(material);
    }

    // 保存为单个 .aymat 文件
    std::string virtualPath = "materials/" + baseName + ".aymat";

    if (!outputDir.empty()) {
        std::string fullPath = outputDir + "/" + virtualPath;
        if (ayt::io::File::exists(fullPath)) {
            // skip
        } else {
            std::vector<UInt8> binaryData;
            if (matFile->saveToBinary(binaryData)) {
                // 计算 GUID（基于材质内容数据，不包含header）
                std::vector<UInt8> matContentData;
                for (const auto& matData : materials) {
                    UInt32 nameLen = static_cast<UInt32>(matData.name.size());
                    UInt32 shaderLen = static_cast<UInt32>(matData.shader.size());
                    matContentData.resize(matContentData.size() + sizeof(UInt32) * 3 + nameLen + shaderLen);
                    UInt8* ptr = matContentData.data() + matContentData.size() - (sizeof(UInt32) * 3 + nameLen + shaderLen);
                    *reinterpret_cast<UInt32*>(ptr) = nameLen; ptr += sizeof(UInt32);
                    memcpy(ptr, matData.name.data(), nameLen); ptr += nameLen;
                    *reinterpret_cast<UInt32*>(ptr) = shaderLen; ptr += sizeof(UInt32);
                    memcpy(ptr, matData.shader.data(), shaderLen); ptr += shaderLen;
                    *reinterpret_cast<UInt32*>(ptr) = static_cast<UInt32>(matData.parameters.size());
                }
                lastGuid = ayt::storage::Guid::computeFromData(matContentData.data(), matContentData.size());
                writeFile(fullPath, binaryData.data(), binaryData.size());
            }
        }
    }

    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Material";
    res.size = static_cast<int64_t>(matFile->sizeInBytes());
    results.push_back(res);

    return results;
}

} // namespace ayt::resource