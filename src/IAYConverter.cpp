#include "IAYConverter.h"
#include "Converter\FBXConverter.h"
#include "Converter\GLTFConverter.h"
#include "Converter\TextureConverter.h"
#include <sstream>

namespace ayt::resource
{

namespace {

bool parseJsonStringField(const std::string& json, size_t searchFrom, const char* key, std::string& out)
{
    const std::string token = std::string("\"") + key + "\": \"";
    const size_t pos = json.find(token, searchFrom);
    if (pos == std::string::npos) {
        return false;
    }
    const size_t valueStart = pos + token.size();
    const size_t valueEnd = json.find('"', valueStart);
    if (valueEnd == std::string::npos) {
        return false;
    }
    out = json.substr(valueStart, valueEnd - valueStart);
    return true;
}

} // namespace

std::unique_ptr<IConverter> IConverter::create(const std::string& sourcePath) {
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
    const size_t depsPos = json.find("\"dependencies\"");
    if (depsPos == std::string::npos) {
        return result;
    }

    size_t pos = json.find('[', depsPos);
    if (pos == std::string::npos) {
        return result;
    }

    while (pos < json.size()) {
        const size_t objStart = json.find('{', pos);
        if (objStart == std::string::npos) {
            break;
        }
        const size_t objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos) {
            break;
        }

        Dependency dep;
        if (parseJsonStringField(json, objStart, "from", dep.from)
            && parseJsonStringField(json, objStart, "to", dep.to)) {
            result.dependencies.push_back(std::move(dep));
        }

        pos = objEnd + 1;
        const size_t nextObj = json.find('{', pos);
        const size_t arrayEnd = json.find(']', pos);
        if (arrayEnd != std::string::npos && (nextObj == std::string::npos || arrayEnd < nextObj)) {
            break;
        }
    }

    return result;
}

} // namespace ayt::resource