#include "AYResource/IConverter.h"
#include <AYAssetFormat/AssetFormat.h>
#if defined(AY_RESOURCE_BUILD_CONVERTER_FACTORY)
#include "AYResource/Converter/FBXConverter.h"
#include "AYResource/Converter/GLTFConverter.h"
#include "AYResource/Converter/TextureConverter.h"
#include "AYResource/Converter/TilemapConverter.h"
#include "AYResource/Converter/AtlasConverter.h"
#include "AYResource/Converter/AudioConverter.h"
#include "AYResource/Converter/AudioContentConverter.h"
#if defined(AY_RESOURCE_HAS_PARTICLE)
#include "AYResource/Converter/ParticleConverter.h"
#endif
#if defined(AY_RESOURCE_HAS_STATS)
#include "AYResource/Converter/StatsConverter.h"
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
#include "AYResource/Converter/DeterministicLevelConverter2D.h"
#endif
#include <cctype>
#else
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#endif

namespace ayt::resource
{

#if !defined(AY_RESOURCE_BUILD_CONVERTER_FACTORY)
const char kFbxImporterContractTag[] =
        "fbx-auto-wrapper-basis-animation-mesh-bind-space-contract-v24";



std::string sourceCoordinatePolicyCacheTag(const SourceCoordinatePolicy& policy)
{
    if (!policy.tag.empty()) {
        return policy.tag
            + (policy.uvOrigin == ImportUvOrigin::BottomLeft
                   ? "|uv-origin=bottom-left"
                   : "|uv-origin=top-left");
    }
    std::ostringstream oss;
    oss << (policy.mode == SourceCoordinateMode::Auto ? "auto" : "manual")
        << ":up=" << static_cast<unsigned>(policy.up)
        << ":forward=" << static_cast<unsigned>(policy.forward)
        << ":hand=" << static_cast<unsigned>(policy.handedness)
        << ":uv-origin=" << static_cast<unsigned>(policy.uvOrigin)
        << ":meters=" << std::setprecision(9) << policy.metersPerUnit;
    return oss.str();
}

#else
std::unique_ptr<IConverter> IConverter::create(const std::string& sourcePath) {
    const auto format = ayt::asset_format::matchPath(sourcePath);
    if(format&&(format->descriptor->id==ayt::asset_format::Id::AudioChartSource||format->descriptor->id==ayt::asset_format::Id::AudioPerformanceSource))return std::make_unique<AudioContentConverter>(sourcePath);
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
    if (format && format->descriptor->id == ayt::asset_format::Id::DeterministicLevelSource)
        return std::make_unique<DeterministicLevelConverter2D>(sourcePath);
#endif
#if defined(AY_RESOURCE_HAS_STATS)
    if (format && format->descriptor->id == ayt::asset_format::Id::StatsSource)
        return std::make_unique<StatsConverter>(sourcePath);
#endif
    if (format && format->descriptor->id == ayt::asset_format::Id::TilemapSource)
        return std::make_unique<TilemapConverter>(sourcePath);
    if (format && format->descriptor->id == ayt::asset_format::Id::AtlasSource)
        return std::make_unique<AtlasConverter>(sourcePath);

    // 根据扩展名判断类型
    size_t dotPos = sourcePath.find_last_of('.');
    std::string ext = (dotPos == std::string::npos) ? "" : sourcePath.substr(dotPos + 1);

    // 转小写
    for (auto& c : ext) c = static_cast<char>(tolower(c));

#if defined(AY_RESOURCE_HAS_PARTICLE)
    if (format && format->descriptor->id == ayt::asset_format::Id::ParticleEffect)
        return std::make_unique<ParticleConverter>(sourcePath);
#endif
    if (ext == "fbx") {
        return std::make_unique<FBXConverter>(sourcePath);
    } else if (ext == "gltf" || ext == "glb") {
        return std::make_unique<GLTFConverter>(sourcePath);
    } else if (ext == "png" || ext == "bmp" || ext == "tga" || ext == "dds"
               || ext == "jpg" || ext == "jpeg") {
        return std::make_unique<TextureConverter>(sourcePath);
    } else if (ext == "wav" || ext == "mp3" || ext == "ogg") {
        return std::make_unique<AudioConverter>(sourcePath);
    }

    return nullptr;
}
#endif

#if !defined(AY_RESOURCE_BUILD_CONVERTER_FACTORY)
std::string ConversionResult::toJson() const {
    using Json = nlohmann::json;
    Json value = {{"resources", Json::array()}, {"dependencies", Json::array()}};
    for (const auto& resource : resources) {
        Json entry = {{"path", resource.path}, {"type", resource.type}, {"size", resource.size}};
        if (!resource.role.empty()) entry["role"] = resource.role;
        value["resources"].push_back(std::move(entry));
    }
    for (const auto& dependency : dependencies)
        value["dependencies"].push_back({{"from", dependency.from}, {"to", dependency.to}});
    if (!textureMode.empty()) value["textureMode"] = textureMode;
    if (!materialPolicyTag.empty()) value["materialPolicyTag"] = materialPolicyTag;
    if (!importerContractTag.empty()) value["importerContractTag"] = importerContractTag;
    if (!sourceCoordinateTag.empty()) value["sourceCoordinateTag"] = sourceCoordinateTag;
    if (!sourceFiles.empty()) {
        value["sourceFiles"] = Json::array();
        for (const auto& source : sourceFiles) value["sourceFiles"].push_back(
            {{"path", source.path}, {"sha256", source.sha256}, {"size", source.size}});
    }
    return value.dump(2) + "\n";
}

ConversionResult ConversionResult::fromJson(const std::string& text) {
    using Json = nlohmann::json;
    ConversionResult result;
    const auto value = Json::parse(text, nullptr, false);
    if (!value.is_object()) return {};
    try {
        result.textureMode = value.value("textureMode", std::string{});
        result.materialPolicyTag = value.value("materialPolicyTag", std::string{});
        result.importerContractTag = value.value("importerContractTag", std::string{});
        result.sourceCoordinateTag = value.value("sourceCoordinateTag", std::string{});
        if (value.contains("sourceFiles")) {
            if (!value.at("sourceFiles").is_array()) return {};
            for (const auto& source : value.at("sourceFiles")) {
                if (source.is_string()) result.sourceFiles.push_back({source.get<std::string>(), {}, 0});
                else result.sourceFiles.push_back({source.at("path").get<std::string>(),
                    source.at("sha256").get<std::string>(), source.at("size").get<std::uint64_t>()});
            }
        }
        if (value.contains("resources") && !value.at("resources").is_array()) return {};
        for (const auto& entry : value.value("resources", Json::array())) {
            ConvertedResource resource;
            resource.path = entry.at("path").get<std::string>();
            resource.type = entry.at("type").get<std::string>();
            resource.role = entry.value("role", std::string{});
            if (entry.contains("size")) {
                const auto& size = entry.at("size");
                if (!size.is_number_integer() || (size.is_number_integer()
                    && !size.is_number_unsigned() && size.get<std::int64_t>() < 0)) return {};
                resource.size = size.get<std::uint64_t>();
            }
            if (resource.path.empty() || resource.path.find('\0') != std::string::npos
                || resource.type.empty() || resource.type.find('\0') != std::string::npos) return {};
            result.resources.push_back(std::move(resource));
        }
        if (value.contains("dependencies")) {
            if (!value.at("dependencies").is_array()) return {};
            for (const auto& entry : value.at("dependencies"))
                result.dependencies.push_back({entry.at("from").get<std::string>(),
                    entry.at("to").get<std::string>()});
        }
    } catch (...) { return {}; }
    return result;
}

#endif

} // namespace ayt::resource
