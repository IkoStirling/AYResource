#include "AYResource/Converter/MaterialConverter.h"
#include "AYResource/VirtualAssetPath.h"
#include "AYIO/File.h"
#include <AYSerializer.h>
#include <AYStorage/Guid.h>
#include <AYLog.h>
#include <cstring>

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
        // F3.5: previously a silent fallback. Now routed through AYLog
        // so missing-shader problems surface in the cook log instead of
        // the material showing up as a duplicated phantom pbr.phoskia in
        // the cooker.
        ayt::log::warn("[MaterialConverter] source '%s' has no 'shader' field; "
                       "falling back to 'simple_lit_shadow.phoskia'",
                       sourcePath.c_str());
        shader = "simple_lit_shadow.phoskia";
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

    // One .aymat per material — runtime Material::load expects the single-
    // material binary (Material::saveToBinary), not MaterialFile multi-pack.
    // Virtual path must match FBXParser mesh.materialSlots.
    for (size_t i = 0; i < materials.size(); i++) {
        const auto& matData = materials[i];

        auto material = std::make_shared<Material>();
        const std::string fallbackName =
            baseName + "_material_" + std::to_string(i);
        material->initialise(
            matData.name.empty() ? fallbackName.c_str() : matData.name.c_str(),
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

        std::vector<UInt8> contentData;
        const std::string name = material->getName();
        const std::string shader = material->getShader();
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
        const ayt::math::FGuid guid =
            ayt::storage::Guid::computeFromData(contentData.data(), contentData.size());
        material->setGuid(guid);
        lastGuid = guid;

        std::vector<UInt8> binaryData;
        if (!material->saveToBinary(binaryData)) {
            ayt::log::warn("[MaterialConverter] saveToBinary failed for material %zu; skipping",
                           i);
            continue;
        }

        const std::string virtualPath = makeMaterialVirtualPath(baseName, i);
        if (!outputDir.empty()) {
            const std::string fullPath = outputDir + "/" + virtualPath;
            if (!writeFile(fullPath, binaryData.data(), binaryData.size())) {
                ayt::log::warn("[MaterialConverter] failed to write %s; skipping",
                               fullPath.c_str());
                continue;
            }
        }

        ConversionResult::ConvertedResource res;
        res.guid = guid;
        res.path = virtualPath;
        res.type = "Material";
        res.size = static_cast<uint64_t>(binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource