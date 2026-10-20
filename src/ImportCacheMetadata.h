#pragma once

#include "AYResource/ImportJob.h"
#include "ImportNativePath.h"
#include <AYAssetFormat/AssetFormat.h>
#include <AYCrypto.h>
#include <AYIO/PathSafety.h>
#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace ayt::resource::detail {
using ImportJson = nlohmann::json;

inline std::string importPathText(const std::filesystem::path& path)
{
    return importPortablePathText(path);
}

inline std::string sourceIdentity(const std::string& source)
{
    return importPathText(std::filesystem::canonical(importNativePath(std::filesystem::u8path(source))));
}

inline bool validOutputNamespace(const std::string& value)
{
    return value.size() <= 80 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

inline bool sameSourceIdentity(const std::string& first, const std::string& second)
{
    if (first == second) return true;
    std::error_code error;
    return std::filesystem::equivalent(std::filesystem::u8path(first), std::filesystem::u8path(second), error) && !error;
}

inline ImportJson importSettings(const ImportOptions& options)
{
    const auto& material = options.materialPolicy;
    const auto& coordinates = options.sourceCoordinates;
    return {{"loadOption", static_cast<int>(options.loadOption)},
        {"cookTextures", options.cookTextures},
        {"requireCharacterAssets", options.requireCharacterAssets},
        {"requireAnimationAssets", options.requireAnimationAssets},
        {"sourceAssetRoot", options.sourceAssetRoot},
        {"outputNamespace", options.outputNamespace},
        {"material", {{"tag", material.tag}, {"opaqueIndices", material.opaqueIndices},
            {"maskIndices", material.maskIndices}, {"blendIndices", material.blendIndices},
            {"doubleSidedIndices", material.doubleSidedIndices},
            {"opaqueNames", material.opaqueNames}, {"maskNames", material.maskNames},
            {"blendNames", material.blendNames}, {"doubleSidedNames", material.doubleSidedNames},
            {"normalMapYSign", material.normalMapYSign}}},
        {"coordinates", {{"mode", static_cast<int>(coordinates.mode)},
            {"up", static_cast<int>(coordinates.up)},
            {"forward", static_cast<int>(coordinates.forward)},
            {"handedness", static_cast<int>(coordinates.handedness)},
            {"uvOrigin", static_cast<int>(coordinates.uvOrigin)},
            {"metersPerUnit", coordinates.metersPerUnit}, {"tag", coordinates.tag}}}};
}

inline int importEnum(const ImportJson& value, int minimum, int maximum)
{
    if (!value.is_number_integer() || value < minimum || value > maximum)
        throw std::runtime_error("invalid import enum representation/range");
    return value.get<int>();
}

inline void decodeImportSettings(const ImportJson& settings, ImportOptions& options)
{
    const auto loadOption = importEnum(settings.at("loadOption"), 0, 2);
    if (loadOption < 0 || loadOption > 2) throw std::runtime_error("invalid import load option");
    options.loadOption = static_cast<IConverter::LoadOption>(loadOption);
    options.cookTextures = settings.at("cookTextures").get<bool>();
    options.requireCharacterAssets = settings.at("requireCharacterAssets").get<bool>();
    options.requireAnimationAssets = settings.at("requireAnimationAssets").get<bool>();
    options.sourceAssetRoot = settings.at("sourceAssetRoot").get<std::string>();
    if (options.sourceAssetRoot.find('\0') != std::string::npos)
        throw std::runtime_error("invalid source asset root");
    options.outputNamespace = settings.value("outputNamespace", std::string{});
    if (!validOutputNamespace(options.outputNamespace)) throw std::runtime_error("invalid import output namespace");
    const auto& material = settings.at("material");
    auto& policy = options.materialPolicy;
    policy.tag = material.at("tag").get<std::string>();
    policy.opaqueIndices = material.at("opaqueIndices").get<std::string>();
    policy.maskIndices = material.at("maskIndices").get<std::string>();
    policy.blendIndices = material.at("blendIndices").get<std::string>();
    policy.doubleSidedIndices = material.at("doubleSidedIndices").get<std::string>();
    policy.opaqueNames = material.at("opaqueNames").get<std::string>();
    policy.maskNames = material.at("maskNames").get<std::string>();
    policy.blendNames = material.at("blendNames").get<std::string>();
    policy.doubleSidedNames = material.at("doubleSidedNames").get<std::string>();
    policy.normalMapYSign = material.at("normalMapYSign").get<float>();
    const auto& coordinates = settings.at("coordinates");
    auto& source = options.sourceCoordinates;
    const int mode = importEnum(coordinates.at("mode"), 0, 1);
    const int up = importEnum(coordinates.at("up"), 0, 5);
    const int forward = importEnum(coordinates.at("forward"), 0, 5);
    const int hand = importEnum(coordinates.at("handedness"), 0, 1);
    const int uv = importEnum(coordinates.at("uvOrigin"), 0, 1);
    if (mode < 0 || mode > 1 || up < 0 || up > 5 || forward < 0 || forward > 5
        || hand < 0 || hand > 1 || uv < 0 || uv > 1)
        throw std::runtime_error("invalid import coordinate settings");
    if (mode == 1 && up / 2 == forward / 2)
        throw std::runtime_error("manual up/forward axes must differ");
    source.mode = static_cast<SourceCoordinateMode>(mode);
    source.up = static_cast<ImportAxis>(up);
    source.forward = static_cast<ImportAxis>(forward);
    source.handedness = static_cast<ImportHandedness>(hand);
    source.uvOrigin = static_cast<ImportUvOrigin>(uv);
    source.metersPerUnit = coordinates.at("metersPerUnit").get<float>();
    source.tag = coordinates.at("tag").get<std::string>();
    if (!std::isfinite(source.metersPerUnit) || source.metersPerUnit < 0
        || !std::isfinite(policy.normalMapYSign))
        throw std::runtime_error("invalid nonfinite import settings");
}

inline ImportJson fingerprintFile(const std::filesystem::path& path)
{
    const auto native = importNativePath(path);
    if (!std::filesystem::is_regular_file(native))
        throw std::runtime_error("missing import dependency: " + importPathText(path));
    auto hash = ayt::crypto::createHash(ayt::crypto::HashAlgo::SHA256);
    if (!hash) throw std::runtime_error("cannot create import SHA256");
    hash->init();
    std::ifstream input(native, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read import dependency: " + importPathText(path));
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t size = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash->update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                static_cast<std::size_t>(count));
            size += static_cast<std::uint64_t>(count);
        }
    }
    if (!input.eof()) throw std::runtime_error("could not finish reading import dependency");
    std::array<std::uint8_t, 64> bytes{};
    hash->final(bytes.data());
    static constexpr char digits[] = "0123456789abcdef";
    std::string digest;
    for (std::size_t index = 0; index < hash->getHashSize(); ++index) {
        digest += digits[bytes[index] >> 4];
        digest += digits[bytes[index] & 15];
    }
    return {{"size", size}, {"sha256", digest}};
}

inline ImportJson readImportJson(const std::string& path)
{
    std::ifstream input(importNativePath(std::filesystem::u8path(path)), std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || input.tellg() > 16 * 1024 * 1024) return {};
    input.seekg(0);
    return ImportJson::parse(input, nullptr, false);
}

inline ConversionResult::SourceFile snapshotImportSource(const std::string& path)
{
    const auto fingerprint = fingerprintFile(std::filesystem::u8path(path));
    return {sourceIdentity(path), fingerprint.at("sha256").get<std::string>(),
        fingerprint.at("size").get<std::uint64_t>()};
}

inline bool matchesImportMetadata(const ImportOptions& options, const ImportJson& value)
{
    try {
        if (!value.is_object() || !value.contains("import")) return options.allowLegacyCache;
        const auto& metadata = value.at("import");
        if (metadata.at("version") != 2
            || !sameSourceIdentity(metadata.at("source").get<std::string>(), sourceIdentity(options.sourcePath))
            || metadata.value("sourceId", std::string{}) != options.sourceId
            || metadata.at("settings") != importSettings(options)
            || metadata.at("inputs").empty() || metadata.at("outputs").empty()) return false;
        for (const auto& entry : metadata.at("inputs")) {
            if (fingerprintFile(std::filesystem::u8path(entry.at("path").get<std::string>()))
                != entry.at("fingerprint")) return false;
        }
        for (const auto& entry : metadata.at("outputs")) {
            const auto name = entry.at("path").get<std::string>();
            if (!ayt::io::path::isPortableRelative(name)) return false;
            std::filesystem::path resolved;
            std::string error;
            if (!ayt::io::path::resolveWithinRoot(std::filesystem::u8path(options.outputDir),
                std::filesystem::u8path(options.outputDir) / std::filesystem::u8path(name), resolved, error))
                return false;
            if (fingerprintFile(resolved)
                != entry.at("fingerprint")) return false;
        }
        return true;
    } catch (...) { return false; }
}

inline void rejectOtherSource(const ImportOptions& options, const ImportJson& value)
{
    if (value.is_object() && value.contains("import")) {
        const auto& metadata = value.at("import");
        const bool explicitlyBound = !options.sourceId.empty()
            && metadata.value("sourceId", std::string{}) == options.sourceId;
        if (metadata.is_object() && metadata.contains("source") && !explicitlyBound
            && !sameSourceIdentity(metadata.at("source").get<std::string>(), sourceIdentity(options.sourcePath)))
            throw std::runtime_error("import basename is already owned by another source; "
                "choose a different destination or source filename");
    }
}

inline void writeImportMetadata(const ImportOptions& options, const ConversionResult& result,
    const std::string& stagingRoot, const std::string& sidecarName)
{
    namespace fs = std::filesystem;
    ImportJson metadata = {{"version", 2}, {"source", sourceIdentity(options.sourcePath)},
        {"sourceId", options.sourceId},
        {"settings", importSettings(options)}, {"inputs", ImportJson::array()},
        {"outputs", ImportJson::array()}};
    std::set<std::string> inputs{sourceIdentity(options.sourcePath)};
    for (const auto& file : result.sourceFiles) {
        const auto current = snapshotImportSource(file.path);
        if (!file.sha256.empty() && (current.sha256 != file.sha256 || current.size != file.size))
            throw std::runtime_error("source dependency changed during import: " + file.path);
        inputs.insert(current.path);
    }
    for (const auto& file : inputs)
        metadata["inputs"].push_back({{"path", file}, {"fingerprint", fingerprintFile(fs::u8path(file))}});
    const auto nativeStage = importNativePath(fs::u8path(stagingRoot));
    for (const auto& file : fs::recursive_directory_iterator(nativeStage)) {
        if (!file.is_regular_file()) continue;
        const auto relative = importPathText(file.path().lexically_relative(nativeStage));
        if (relative == sidecarName) continue;
        metadata["outputs"].push_back({{"path", relative}, {"fingerprint", fingerprintFile(file.path())}});
    }
    auto value = ImportJson::parse(result.toJson());
    value["import"] = std::move(metadata);
    const auto text = value.dump(2) + "\n";
    std::ofstream output(nativeStage / fs::u8path(sidecarName),
        std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) throw std::runtime_error("could not write import metadata");
}

} // namespace ayt::resource::detail
