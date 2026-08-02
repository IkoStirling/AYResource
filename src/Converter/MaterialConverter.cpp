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

// Helper: read a float parameter that may be either a scalar (single value)
// or an array [x, y, ...]. Returns false if the field doesn't exist.
// For scalar form, only out[0] is populated (caller must zero the rest).
// For array form, the JSON must have at least `count` elements.
static bool tryReadFloatArray(ayt::serializer::ISerializer& s, const char* name, Float32* out, int count) {
    if (!s.isFieldPending(name)) {
        return false;
    }

    // peekFieldTokenType: returns the JSON value's token type.
    // TokenType enum (see SerializerCore.h):
    //   None=0, ObjectBegin=1, ObjectEnd=2, ArrayBegin=3, ArrayEnd=4,
    //   Field=5, String=6, Integer=7, Float=8, Bool=9, Null=10
    // ArrayBegin (3) means the JSON value is a real array; anything else
    // (Field/String/Integer/Float/...) is a scalar or null.
    const int tok = s.peekFieldTokenType(name);
    if (tok == static_cast<int>(ayt::serializer::TokenType::ArrayBegin)) {
        s.beginArray(name);
        for (int i = 0; i < count; i++) {
            s.field(nullptr, out[i]);
        }
        s.endArray();
    } else {
        // Scalar (Float/Integer/Field/etc.). Read into out[0].
        s.field(name, out[0]);
    }
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
    material->initialise(name, shader);

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
    if (serializer->isFieldPending("mainTexture")) {
        serializer->field("mainTexture", texPath);
        if (!texPath.empty()) {
            material->setTexture("mainTexture", texPath.c_str());
        }
    }

    serializer->endObject(); // parameters
    serializer->endObject(); // root

    material->setLoaded(true);

    // F2.1: walk parameters via the public accessor (no friend edge).
    // The accessor yields each parameter's name + type + value bytes.
    std::vector<UInt8> contentData;
    contentData.insert(contentData.end(), name.begin(), name.end());
    contentData.insert(contentData.end(), shader.begin(), shader.end());
    material->forEachParameterHashSink(
        [&contentData](const std::string& pname, MaterialParamType ptype,
                       const UInt8* bytes, size_t n) {
            const UInt32 nameLen = static_cast<UInt32>(pname.size());
            contentData.resize(contentData.size() + sizeof(UInt32) + nameLen);
            UInt8* ptr = contentData.data() + contentData.size() - nameLen;
            *reinterpret_cast<UInt32*>(ptr - sizeof(UInt32)) = nameLen;
            memcpy(ptr, pname.data(), nameLen);
            contentData.push_back(static_cast<UInt8>(ptype));
            contentData.insert(contentData.end(), bytes, bytes + n);
        });
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
        // Always overwrite so reruns (and updates after schema/source changes)
        // reflect the current convert() result instead of stale content.
        writeFile(fullPath, binaryData.data(), binaryData.size());
    }

    // 构建资源信息
    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Material";
    res.size = static_cast<uint64_t>(binaryData.size());
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
        material->initialise(
            matData.name.empty() ? baseName : matData.name.c_str(),
            matData.shader.c_str());

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

        material->setLoaded(true);
        matFile->addMaterial(material);
    }

    // 保存为单个 .aymat 文件
    std::string virtualPath = "materials/" + baseName + ".aymat";

    if (!outputDir.empty()) {
        std::string fullPath = outputDir + "/" + virtualPath;
        // F3.2: always overwrite so re-runs (or schema/source changes)
        // reflect the current convert() result instead of stale content.
        // The previous `if (exists) skip` left a cooked .aymat on disk
        // even after the source JSON's parameters changed.
        std::vector<UInt8> binaryData;
        if (matFile->saveToBinary(binaryData)) {
            // F1.5b: GUID hashes full content (name+shader+param
            // names/types/values+texture paths). v0 hash omitted
            // parameter values + texture paths so two materials
            // with same name+shader+param-type-list but different
            // colors or textures shared a GUID.
            std::vector<UInt8> matContentData;
            for (const auto& matData : materials) {
                UInt32 nameLen = static_cast<UInt32>(matData.name.size());
                UInt32 shaderLen = static_cast<UInt32>(matData.shader.size());
                matContentData.resize(matContentData.size() + sizeof(UInt32) * 2 + nameLen + shaderLen);
                UInt8* ptr = matContentData.data() + matContentData.size() - (sizeof(UInt32) * 2 + nameLen + shaderLen);
                *reinterpret_cast<UInt32*>(ptr) = nameLen; ptr += sizeof(UInt32);
                memcpy(ptr, matData.name.data(), nameLen); ptr += nameLen;
                *reinterpret_cast<UInt32*>(ptr) = shaderLen; ptr += sizeof(UInt32);
                memcpy(ptr, matData.shader.data(), shaderLen); ptr += shaderLen;
                for (const auto& param : matData.parameters) {
                    UInt32 pnameLen = static_cast<UInt32>(param.name.size());
                    matContentData.resize(matContentData.size() + sizeof(UInt32) + pnameLen + sizeof(UInt8));
                    UInt8* pp = matContentData.data() + matContentData.size() - (sizeof(UInt32) + pnameLen + sizeof(UInt8));
                    *reinterpret_cast<UInt32*>(pp) = pnameLen; pp += sizeof(UInt32);
                    memcpy(pp, param.name.data(), pnameLen); pp += pnameLen;
                    *reinterpret_cast<UInt8*>(pp) = static_cast<UInt8>(param.type);
                    switch (param.type) {
                    case MaterialParamType::Float:
                        matContentData.insert(matContentData.end(),
                            reinterpret_cast<const UInt8*>(&param.floatValue),
                            reinterpret_cast<const UInt8*>(&param.floatValue) + sizeof(Float32));
                        break;
                    case MaterialParamType::Float2:
                        matContentData.insert(matContentData.end(),
                            reinterpret_cast<const UInt8*>(param.float2Value),
                            reinterpret_cast<const UInt8*>(param.float2Value) + sizeof(Float32) * 2);
                        break;
                    case MaterialParamType::Float3:
                        matContentData.insert(matContentData.end(),
                            reinterpret_cast<const UInt8*>(param.float3Value),
                            reinterpret_cast<const UInt8*>(param.float3Value) + sizeof(Float32) * 3);
                        break;
                    case MaterialParamType::Float4:
                        matContentData.insert(matContentData.end(),
                            reinterpret_cast<const UInt8*>(param.float4Value),
                            reinterpret_cast<const UInt8*>(param.float4Value) + sizeof(Float32) * 4);
                        break;
                    case MaterialParamType::Float4x4:
                        matContentData.insert(matContentData.end(),
                            reinterpret_cast<const UInt8*>(param.matrixValue),
                            reinterpret_cast<const UInt8*>(param.matrixValue) + sizeof(Float32) * 16);
                        break;
                    case MaterialParamType::Int:
                        matContentData.insert(matContentData.end(),
                            reinterpret_cast<const UInt8*>(&param.intValue),
                            reinterpret_cast<const UInt8*>(&param.intValue) + sizeof(Int32));
                        break;
                    case MaterialParamType::Bool:
                        matContentData.push_back(param.boolValue ? 1 : 0);
                        break;
                    case MaterialParamType::Texture2D:
                    case MaterialParamType::Texture3D:
                    case MaterialParamType::TextureCube:
                        matContentData.insert(matContentData.end(),
                            param.texturePath.begin(), param.texturePath.end());
                        break;
                    }
                }
            }
            lastGuid = ayt::storage::Guid::computeFromData(matContentData.data(), matContentData.size());
            // F3.1: drop the result entry if the write fails so callers
            // don't see a perfect-looking record for a file that isn't
            // actually on disk.
            if (!writeFile(fullPath, binaryData.data(), binaryData.size())) {
                ayt::log::warn("[MaterialConverter] failed to write %s; skipping",
                               fullPath.c_str());
                return results;
            }
        }
    }

    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Material";
    res.size = static_cast<uint64_t>(matFile->sizeInBytes());
    results.push_back(res);

    return results;
}

} // namespace ayt::resource