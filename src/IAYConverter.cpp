#include "IAYConverter.h"
#include "Converter\FBXConverter.h"
#include "Converter\GLTFConverter.h"
#include "Converter\TextureConverter.h"
#include "Converter\TilemapConverter.h"
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace ayt::resource
{

namespace {

bool parseJsonStringField(const std::string& json, size_t searchFrom, const char* key, std::string& out)
{
    // Accept both `"key": "` and `"key":"` (space optional after colon).
    const std::string keyTok = std::string("\"") + key + "\":";
    size_t pos = json.find(keyTok, searchFrom);
    if (pos == std::string::npos) {
        return false;
    }
    pos += keyTok.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    const size_t valueStart = pos + 1;
    const size_t valueEnd = json.find('"', valueStart);
    if (valueEnd == std::string::npos) {
        return false;
    }
    out = json.substr(valueStart, valueEnd - valueStart);
    return true;
}

// F2.3: ConvertedResource.size is now uint64_t; parse with strtoull.
bool parseJsonUInt64Field(const std::string& json, size_t searchFrom, const char* key, uint64_t& out)
{
    const std::string keyTok = std::string("\"") + key + "\":";
    size_t pos = json.find(keyTok, searchFrom);
    if (pos == std::string::npos) {
        return false;
    }
    pos += keyTok.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    if (pos >= json.size() || (json[pos] < '0' || json[pos] > '9')) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) {
        return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

// Walk one JSON array of objects starting at `arrayOpen` ('['). Invokes
// `fn(objStart)` for each `{...}` until the matching `]`.
template <typename Fn>
void forEachJsonObjectInArray(const std::string& json, size_t arrayOpen, Fn&& fn)
{
    size_t pos = arrayOpen;
    while (pos < json.size()) {
        const size_t objStart = json.find('{', pos);
        if (objStart == std::string::npos) {
            break;
        }
        const size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos) {
            break;
        }
        fn(objStart);
        pos = objEnd + 1;
        const size_t nextObj = json.find('{', pos);
        const size_t arrayEnd = json.find(']', pos);
        if (arrayEnd != std::string::npos
            && (nextObj == std::string::npos || arrayEnd < nextObj)) {
            break;
        }
    }
}

} // namespace

std::unique_ptr<IConverter> IConverter::create(const std::string& sourcePath) {
    // CM-2: .aytilemap.json MUST be dispatched before the generic
    // extension check — its last extension is "json" and the check below
    // would silently return nullptr. Case-insensitive suffix match.
    {
        const std::string lower = sourcePath;
        std::string suffix;
        if (lower.size() >= 15) {
            suffix = lower.substr(lower.size() - 15);
        }
        for (auto& c : suffix) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        if (suffix == ".aytilemap.json") {
            return std::make_unique<TilemapConverter>(sourcePath);
        }
    }

    // 根据扩展名判断类型
    size_t dotPos = sourcePath.find_last_of('.');
    std::string ext = (dotPos == std::string::npos) ? "" : sourcePath.substr(dotPos + 1);

    // 转小写
    for (auto& c : ext) c = static_cast<char>(tolower(c));

    if (ext == "fbx") {
        return std::make_unique<FBXConverter>(sourcePath);
    } else if (ext == "gltf" || ext == "glb") {
        return std::make_unique<GLTFConverter>(sourcePath);
    } else if (ext == "png" || ext == "bmp" || ext == "tga" || ext == "dds") {
        return std::make_unique<TextureConverter>(sourcePath);
    }

    return nullptr;
}

std::string ConversionResult::toJson() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"resources\": [\n";
    for (size_t i = 0; i < resources.size(); i++) {
        const auto& res = resources[i];
        oss << "    {\"path\": \"" << res.path << "\", \"type\": \"" << res.type << "\", \"size\": " << res.size << "}";
        if (i + 1 < resources.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ],\n";
    oss << "  \"dependencies\": [\n";
    for (size_t i = 0; i < dependencies.size(); i++) {
        const auto& dep = dependencies[i];
        oss << "    {\"from\": \"" << dep.from << "\", \"to\": \"" << dep.to << "\"}";
        if (i + 1 < dependencies.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
}

ConversionResult ConversionResult::fromJson(const std::string& json) {
    ConversionResult result;

    // Resources MUST be parsed — Importer cache reuse checks hasMesh/hasSkel
    // on this array. A previous implementation only read dependencies, so
    // every restart forced a full FBX reconvert ("missing Mesh/Skeleton").
    const size_t resPos = json.find("\"resources\"");
    if (resPos != std::string::npos) {
        const size_t arrayOpen = json.find('[', resPos);
        if (arrayOpen != std::string::npos) {
            forEachJsonObjectInArray(json, arrayOpen, [&](size_t objStart) {
                ConvertedResource res;
                if (!parseJsonStringField(json, objStart, "path", res.path)
                    || !parseJsonStringField(json, objStart, "type", res.type)) {
                    return;
                }
                (void)parseJsonUInt64Field(json, objStart, "size", res.size);
                result.resources.push_back(std::move(res));
            });
        }
    }

    const size_t depsPos = json.find("\"dependencies\"");
    if (depsPos != std::string::npos) {
        const size_t arrayOpen = json.find('[', depsPos);
        if (arrayOpen != std::string::npos) {
            forEachJsonObjectInArray(json, arrayOpen, [&](size_t objStart) {
                Dependency dep;
                if (parseJsonStringField(json, objStart, "from", dep.from)
                    && parseJsonStringField(json, objStart, "to", dep.to)) {
                    result.dependencies.push_back(std::move(dep));
                }
            });
        }
    }

    return result;
}

} // namespace ayt::resource
