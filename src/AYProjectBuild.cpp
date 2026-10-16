#include "AYResource/ProjectBuild.h"
#include <AYResource/Converter/AudioContentConverter.h>
#include <AYResource/AudioContent.h>
#include "ProjectPublishRetry.h"
#include "ProjectShaderProduction.h"
#include "ProjectLogiaContent.h"

#include "AYResource/ImportJob.h"
#include "AYResource/ResourceBootstrap.h"
#include "AYResource/ResourceRegistry.h"
#include <AYAssetFormat/AssetFormat.h>

#include <AYCrypto.h>
#include <AYIO/File.h>
#include <AYIO/FileTransaction.h>
#include <AYIO/PathSafety.h>
#include <AYPlatform/ChildProcess.h>
#include <AYStorage/IPackageWriter.h>
#include <AYStorage/IStorageDatabase.h>
#include <nlohmann/json.hpp>
#if defined(AY_RESOURCE_HAS_PARTICLE)
#include <AYParticle/EffectPackaging.h>
#endif
#if defined(AY_RESOURCE_HAS_STATS)
#include <AYStats/RecipeAsset.h>
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
#include <AYEntity/DeterministicLevelCooked2D.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace ayt::resource {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr const char* kPackageManifest = "build-manifest.json";

fs::path nativeFilesystemPath(const fs::path& path)
{
#if defined(_WIN32)
    // Only physical I/O uses this spelling. Keep project-relative names, cache
    // keys, containment preflight and published identities in their original form.
    // Cache hash components can exceed MAX_PATH before atomicWrite is reached.
    const auto native = fs::absolute(path).lexically_normal().make_preferred().native();
    if (native.rfind(L"\\\\?\\", 0) == 0) return fs::path(native);
    if (native.rfind(L"\\\\", 0) == 0)
        return fs::path(L"\\\\?\\UNC\\" + native.substr(2));
    return fs::path(L"\\\\?\\" + native);
#else
    return path;
#endif
}

std::string portable(fs::path value)
{
    return value.generic_string();
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return value;
}

bool portableRelative(const std::string& value)
{
    if (value.empty()) return false;
    const fs::path path(value);
    if (path.is_absolute() || path.has_root_directory() || path.has_root_name()) {
        return false;
    }
    for (const fs::path& part : path) {
        if (part == "..") return false;
    }
    return true;
}

bool nestedPath(const fs::path& candidate, const fs::path& parent)
{
    const fs::path relative = candidate.lexically_normal().lexically_relative(
        parent.lexically_normal());
    return !relative.empty() && !relative.is_absolute()
        && *relative.begin() != "..";
}

bool safeChunkName(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return std::isalnum(byte) != 0 || ch == '-' || ch == '_' || ch == '.';
    });
}

bool isUnder(const fs::path& root, const fs::path& candidate)
{
    fs::path resolved;
    std::string error;
    return ayt::io::path::resolveWithinRoot(root, candidate, resolved, error);
}

const char* transformName(ProjectAssetTransform value)
{
    switch (value) {
    case ProjectAssetTransform::Auto: return "auto";
    case ProjectAssetTransform::Raw: return "raw";
    case ProjectAssetTransform::Cook: return "cook";
    case ProjectAssetTransform::Exclude: return "exclude";
    }
    return "unknown";
}

const char* storageName(ProjectAssetStorage value)
{
    return value == ProjectAssetStorage::Pak ? "pak" : "loose";
}

const char* policyName(ProjectCookPolicy value)
{
    switch (value) {
    case ProjectCookPolicy::Auto: return "auto";
    case ProjectCookPolicy::CacheOnly: return "cacheOnly";
    case ProjectCookPolicy::Force: return "force";
    case ProjectCookPolicy::Never: return "never";
    }
    return "unknown";
}

bool parseTransform(const std::string& text, ProjectAssetTransform& value)
{
    const std::string normalized = lower(text);
    if (normalized == "auto") value = ProjectAssetTransform::Auto;
    else if (normalized == "raw") value = ProjectAssetTransform::Raw;
    else if (normalized == "cook") value = ProjectAssetTransform::Cook;
    else if (normalized == "exclude") value = ProjectAssetTransform::Exclude;
    else return false;
    return true;
}

bool parseStorage(const std::string& text, ProjectAssetStorage& value,
                  std::string* chunk = nullptr)
{
    const std::string normalized = lower(text);
    if (normalized == "loose") {
        value = ProjectAssetStorage::Loose;
        return true;
    }
    if (normalized == "pak") {
        value = ProjectAssetStorage::Pak;
        return true;
    }
    if (normalized.rfind("pak:", 0) == 0 && normalized.size() > 4) {
        value = ProjectAssetStorage::Pak;
        if (chunk != nullptr) *chunk = text.substr(4);
        return true;
    }
    return false;
}

bool parsePolicy(const std::string& text, ProjectCookPolicy& value)
{
    const std::string normalized = lower(text);
    if (normalized == "auto") value = ProjectCookPolicy::Auto;
    else if (normalized == "cacheonly" || normalized == "cache-only") {
        value = ProjectCookPolicy::CacheOnly;
    } else if (normalized == "force") value = ProjectCookPolicy::Force;
    else if (normalized == "never" || normalized == "no-cook") {
        value = ProjectCookPolicy::Never;
    } else return false;
    return true;
}

std::regex globRegex(const std::string& glob)
{
    std::string expression = "^";
    for (std::size_t index = 0; index < glob.size(); ++index) {
        const char ch = glob[index];
        if (ch == '*') {
            if (index + 1 < glob.size() && glob[index + 1] == '*') {
                ++index;
                if (index + 1 < glob.size() && glob[index + 1] == '/') {
                    ++index;
                    expression += "(?:.*/)?";
                } else {
                    expression += ".*";
                }
            } else {
                expression += "[^/]*";
            }
        } else if (ch == '?') {
            expression += "[^/]";
        } else {
            if (std::string_view(".^$|()[]{}+\\").find(ch)
                != std::string_view::npos) {
                expression.push_back('\\');
            }
            expression.push_back(ch == '\\' ? '/' : ch);
        }
    }
    expression += "$";
    return std::regex(expression, std::regex::ECMAScript | std::regex::icase);
}

bool matches(const std::string& pattern, const std::string& logical)
{
    try {
        return std::regex_match(logical, globRegex(pattern));
    } catch (const std::regex_error&) {
        return false;
    }
}

void diagnostic(std::vector<ProjectBuildDiagnostic>& target,
                ProjectBuildDiagnostic::Severity severity,
                std::string asset, std::string message);

bool isBakedSkeletonPath(const fs::path& path)
{
    const std::string filename = lower(path.filename().string());
    return filename.ends_with(".baked.skl") || filename.ends_with(".baked.ayskel");
}

bool isBakedSkeletonArtifact(const fs::path& path)
{
    const std::string filename = lower(path.filename().string());
    const std::string extension = lower(path.extension().string());
    return filename.find(".baked.") != std::string::npos
        && (ayt::asset_format::matchesPath(path.string(), ayt::asset_format::Id::Skeleton)
            || ayt::asset_format::matchesPath(path.string(), ayt::asset_format::Id::Animation)
            || ayt::asset_format::isCanonicalPath(path.string(), ayt::asset_format::Id::Mesh)
            || ayt::asset_format::matchesPath(path.string(), ayt::asset_format::Id::SkeletonMask));
}

bool isSkeletonAuthoringMetadata(const fs::path& path)
{
    const std::string filename = lower(path.filename().string());
    const std::string extension = lower(path.extension().string());
    return ayt::asset_format::matchesPath(path.string(), ayt::asset_format::Id::RigProfile)
        || filename.ends_with(".bake-plan.json")
        || filename.ends_with(".bake-result.json");
}

std::string skeletonSourceFingerprint(const fs::path& path)
{
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) return {};
    const auto modified = fs::last_write_time(path, error);
    if (error) return std::to_string(size);
    return std::to_string(size) + ":"
        + std::to_string(modified.time_since_epoch().count());
}

std::string fnvHex(const char* prefix, const std::string& value)
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= prime;
    }
    std::ostringstream text;
    text << prefix << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}

fs::path resolvedProfileReference(const Json& object, const char* key,
                                  const fs::path& profilePath)
{
    fs::path result = object.value(key, std::string{});
    if (!result.empty() && result.is_relative()) {
        result = profilePath.parent_path() / result;
    }
    std::error_code error;
    const fs::path absolute = fs::absolute(result, error);
    return (error ? result : absolute).lexically_normal();
}

std::string rigProfileFingerprint(const Json& profile,
                                  const std::string& sourceFingerprint,
                                  const fs::path& profilePath)
{
    static constexpr std::string_view roles[] = {
        "sceneRoot", "motionRoot", "hips", "spine", "chest", "upperChest",
        "neck", "head", "leftEye", "rightEye", "jaw", "leftShoulder",
        "leftUpperArm", "leftLowerArm", "leftHand", "rightShoulder",
        "rightUpperArm", "rightLowerArm", "rightHand", "leftUpperLeg",
        "leftLowerLeg", "leftFoot", "leftToes", "rightUpperLeg",
        "rightLowerLeg", "rightFoot", "rightToes", "leftThumbMetacarpal",
        "leftThumbProximal", "leftThumbDistal", "leftIndexProximal",
        "leftIndexIntermediate", "leftIndexDistal", "leftMiddleProximal",
        "leftMiddleIntermediate", "leftMiddleDistal", "leftRingProximal",
        "leftRingIntermediate", "leftRingDistal", "leftLittleProximal",
        "leftLittleIntermediate", "leftLittleDistal", "rightThumbMetacarpal",
        "rightThumbProximal", "rightThumbDistal", "rightIndexProximal",
        "rightIndexIntermediate", "rightIndexDistal", "rightMiddleProximal",
        "rightMiddleIntermediate", "rightMiddleDistal", "rightRingProximal",
        "rightRingIntermediate", "rightRingDistal", "rightLittleProximal",
        "rightLittleIntermediate", "rightLittleDistal",
    };
    const Json profileRoles = profile.value("roles", Json::object());
    const std::string kind = profile.value("kind", std::string{"mapping"});
    const Json target = profile.value("target", Json::object());
    const Json targetRoles = target.value("roles", Json::object());
    const Json output = profile.value("output", Json::object());
    const Json corrections = profile.value("corrections", Json::object());
    const fs::path targetPath = kind == "retarget"
        ? resolvedProfileReference(target, "skeleton", profilePath)
        : fs::path{};
    const std::string outputMode = kind == "retarget"
        ? output.value("mode", std::string{"BakeToTarget"})
        : "SemanticNormalize";
    const std::string platform = kind == "retarget"
        ? output.value("platform", std::string{"default"}) : std::string{};
    std::string canonical = "v1|" + sourceFingerprint + "|native="
        + (profile.value("native", false) ? "1" : "0")
        + "|custom="
        + (profile.value("strategy", std::string{}) == "custom" ? "1" : "0")
        + "|kind=" + kind
        + "|target=" + portable(targetPath)
        + "|targetFingerprint=" + skeletonSourceFingerprint(targetPath)
        + "|outputMode=" + outputMode
        + "|platform=" + platform;
    const auto appendQuaternion = [&canonical](const Json& value) {
        std::array<float, 4> quaternion{0.0f, 0.0f, 0.0f, 1.0f};
        if (value.is_array() && value.size() == quaternion.size()) {
            for (std::size_t index = 0; index < quaternion.size(); ++index) {
                if (value[index].is_number()) {
                    quaternion[index] = value[index].get<float>();
                }
            }
        }
        std::ostringstream encoded;
        encoded << std::setprecision(9)
            << quaternion[0] << "," << quaternion[1] << ","
            << quaternion[2] << "," << quaternion[3];
        canonical += "|" + encoded.str();
    };
    for (const std::string_view role : roles) {
        canonical += "|";
        canonical += role;
        canonical += "=";
        const auto value = profileRoles.find(std::string(role));
        canonical += value != profileRoles.end() && value->is_object()
            ? value->value("bonePath", std::string{"-"}) : "-";
        canonical += "->";
        const auto targetValue = targetRoles.find(std::string(role));
        canonical += targetValue != targetRoles.end()
            && targetValue->is_object()
            ? targetValue->value("bonePath", std::string{"-"}) : "-";
        const auto correction = corrections.find(std::string(role));
        const Json empty = Json::object();
        const Json& correctionValue = correction != corrections.end()
            && correction->is_object() ? *correction : empty;
        appendQuaternion(correctionValue.value(
            "sourceReferenceOffset", Json::array()));
        appendQuaternion(correctionValue.value(
            "targetReferenceOffset", Json::array()));
        appendQuaternion(correctionValue.value(
            "axisCorrection", Json::array()));
    }
    return fnvHex("rig-input-", canonical);
}

std::string rigBakeScope(const Json& profile, const fs::path& profilePath)
{
    if (profile.value("kind", std::string{}) != "retarget") return {};
    const Json target = profile.value("target", Json::object());
    const Json output = profile.value("output", Json::object());
    return fnvHex("rt-", portable(resolvedProfileReference(
        target, "skeleton", profilePath)) + "|"
        + output.value("mode", std::string{"BakeToTarget"}) + "|"
        + output.value("platform", std::string{"default"}));
}

struct SkeletonReleaseGateResult {
    bool valid = false;
    std::unordered_set<std::string> excludedPaths;
    std::unordered_set<std::string> allowedPaths;
};

SkeletonReleaseGateResult validateSkeletonReleaseGate(
    const fs::path& skeletonPath, const fs::path& assetsRoot,
    std::vector<ProjectBuildDiagnostic>& diagnostics)
{
    SkeletonReleaseGateResult result;
    const std::string logical = portable(fs::relative(skeletonPath, assetsRoot));
    const auto reject = [&](std::string code, std::string message) {
        diagnostic(diagnostics, ProjectBuildDiagnostic::Severity::Error,
            logical, "[" + std::move(code) + "] " + std::move(message));
    };
    fs::path mappingPath = skeletonPath;
    mappingPath.replace_extension(ayt::asset_format::suffix(ayt::asset_format::Id::RigProfile));
    if (!fs::is_regular_file(mappingPath)) {
        fs::path legacyPath = skeletonPath;
        legacyPath.replace_extension(".ayrig");
        fs::path oldestPath = skeletonPath;
        oldestPath.replace_extension(".aysmap");
        if (fs::is_regular_file(legacyPath) || fs::is_regular_file(oldestPath)) {
            reject("skeletonRigProfileMigrationRequired",
                "Legacy Rig Profile must be opened and saved as .rig before packaging.");
        } else {
            reject("skeletonRigProfileMissing",
                "Source skeleton has no Rig Profile resource.");
        }
        return result;
    }

    Json mapping;
    try {
        const std::string text = ayt::io::File::readAllText(mappingPath.string());
        mapping = Json::parse(text);
    } catch (const std::exception& exception) {
        reject("skeletonRigProfileInvalid",
            std::string("Rig Profile is unreadable: ") + exception.what());
        return result;
    }
    if (mapping.value("type", std::string{}) != "RigProfile"
        || mapping.value("version", 0u) != 1u
        || (mapping.value("kind", std::string{}) != "mapping"
            && mapping.value("kind", std::string{}) != "retarget")
        || mapping.value("id", std::string{}).empty()) {
        reject("skeletonRigProfileInvalid",
            "Rig Profile schema is unsupported or incomplete.");
        return result;
    }
    const Json source = mapping.value("source", Json::object());
    const std::string skeletonReference = source.value(
        "skeleton", std::string{});
    fs::path referenced = skeletonReference;
    if (referenced.is_relative()) referenced = mappingPath.parent_path() / referenced;
    std::error_code referenceError;
    if (fs::weakly_canonical(referenced, referenceError)
        != fs::weakly_canonical(skeletonPath)) {
        reject("skeletonRigProfileMismatch",
            "Rig Profile references a different source skeleton.");
        return result;
    }
    static constexpr std::array<const char*, 15> requiredRoles = {
        "hips", "spine", "head",
        "leftUpperArm", "leftLowerArm", "leftHand",
        "rightUpperArm", "rightLowerArm", "rightHand",
        "leftUpperLeg", "leftLowerLeg", "leftFoot",
        "rightUpperLeg", "rightLowerLeg", "rightFoot",
    };
    const Json roles = mapping.value("roles", Json::object());
    for (const char* role : requiredRoles) {
        const auto value = roles.find(role);
        if (value == roles.end() || !value->is_object()
            || value->value("bonePath", std::string{}).empty()) {
            reject("skeletonRigProfileIncomplete",
                std::string("Required humanoid role is not mapped: ") + role);
            return result;
        }
    }
    const bool retarget = mapping.value("kind", std::string{}) == "retarget";
    if (retarget) {
        const Json target = mapping.value("target", Json::object());
        const fs::path targetSkeleton = resolvedProfileReference(
            target, "skeleton", mappingPath);
        if (!fs::is_regular_file(targetSkeleton)
            || target.value("fingerprint", std::string{})
                != skeletonSourceFingerprint(targetSkeleton)) {
            reject("skeletonRetargetTargetStale",
                "Retarget target skeleton is missing or changed after the Rig Profile was saved.");
            return result;
        }
        const Json targetRoles = target.value("roles", Json::object());
        for (const char* role : requiredRoles) {
            const auto value = targetRoles.find(role);
            if (value == targetRoles.end() || !value->is_object()
                || value->value("bonePath", std::string{}).empty()) {
                reject("skeletonRetargetProfileIncomplete",
                    std::string("Required target humanoid role is not mapped: ")
                        + role);
                return result;
            }
        }
        const Json output = mapping.value("output", Json::object());
        if (output.value("mode", std::string{}) != "BakeToTarget"
            || output.value("platform", std::string{}).empty()) {
            reject("skeletonRetargetProfileInvalid",
                "Retarget output mode or platform is missing or unsupported.");
            return result;
        }
    }
    const std::string fingerprint = skeletonSourceFingerprint(skeletonPath);
    if (fingerprint.empty()
        || source.value("fingerprint", std::string{}) != fingerprint) {
        reject("skeletonRigProfileStale",
            "Source skeleton changed after its Rig Profile was saved.");
        return result;
    }
    const std::string profileFingerprint = rigProfileFingerprint(
        mapping, fingerprint, mappingPath);

    const std::string scope = rigBakeScope(mapping, mappingPath);
    const fs::path receiptPath = skeletonPath.parent_path() / "Baked"
        / (skeletonPath.stem().string()
            + (scope.empty() ? std::string{} : "." + scope)
            + ".bake-result.json");
    Json receipt;
    try {
        const std::string text = ayt::io::File::readAllText(receiptPath.string());
        receipt = Json::parse(text);
    } catch (const std::exception& exception) {
        reject("skeletonBakeReceiptMissing",
            std::string("Bake result manifest is missing or unreadable: ")
                + exception.what());
        return result;
    }
    if (receipt.value("type", std::string{}) != "SkeletonBakeResult"
        || receipt.value("version", 0u) != 2u
        || receipt.value("sourceFingerprint", std::string{}) != fingerprint
        || receipt.value("profileFingerprint", std::string{})
            != profileFingerprint
        || receipt.value("scope", std::string{}) != scope
        || receipt.value("outputMode", std::string{})
            != (retarget ? "BakeToTarget" : "SemanticNormalize")
        || receipt.value("platform", std::string{})
            != (retarget
                ? mapping.value("output", Json::object()).value(
                    "platform", std::string{"default"})
                : std::string{})) {
        reject("skeletonBakeReceiptInvalid",
            "Bake result manifest does not match the current skeleton and Rig Profile.");
        return result;
    }
    const Json outputs = receipt.value("outputs", Json::array());
    if (!outputs.is_array() || outputs.empty()) {
        reject("skeletonBakeOutputMissing",
            "Bake result manifest contains no outputs.");
        return result;
    }
    std::unordered_set<std::string> outputPaths;
    for (const Json& value : outputs) {
        if (!value.is_string()) {
            reject("skeletonBakeOutputInvalid",
                "Bake result manifest contains an invalid output path.");
            return result;
        }
        const fs::path output = fs::absolute(
            fs::path(value.get<std::string>())).lexically_normal();
        if (!fs::is_regular_file(output) || !isUnder(assetsRoot, output)) {
            reject("skeletonBakeOutputMissing",
                "Baked output is missing or outside the package asset root: "
                    + portable(output));
            return result;
        }
        const std::string normalized = portable(output);
        if (!outputPaths.insert(normalized).second) {
            reject("skeletonBakeOutputInvalid",
                "Bake result manifest contains a duplicate output path: "
                    + normalized);
            return result;
        }
        result.allowedPaths.insert(normalized);
    }

    const Json dependencies = receipt.value("dryRun", Json::object())
        .value("dependencies", Json::array());
    if (!dependencies.is_array()) {
        reject("skeletonBakeDependencyClosureInvalid",
            "Bake receipt has no dependency closure.");
        return result;
    }
    std::unordered_map<std::string, std::string> requiredDependencies;
    for (const Json& dependency : dependencies) {
        if (!dependency.is_object()) {
            reject("skeletonBakeDependencyClosureInvalid",
                "Bake dependency entry is invalid.");
            return result;
        }
        const std::string kind = dependency.value("kind", std::string{});
        const std::string impact = dependency.value("impact", std::string{});
        const fs::path source = fs::absolute(fs::path(
            dependency.value("path", std::string{}))).lexically_normal();
        if ((kind != "animation" && kind != "mesh"
                && kind != "skeletonMask")
            || impact != "affected" || !fs::is_regular_file(source)
            || !isUnder(assetsRoot, source)) {
            reject("skeletonBakeDependencyClosureInvalid",
                "Bake dependency is unsupported, blocked, missing, or outside the asset root: "
                    + portable(source));
            return result;
        }
        const std::string key = kind + "|" + portable(source);
        if (!requiredDependencies.emplace(key, portable(source)).second) {
            reject("skeletonBakeDependencyClosureInvalid",
                "Bake dependency closure contains a duplicate entry: "
                    + portable(source));
            return result;
        }
        result.excludedPaths.insert(portable(source));
    }

    const Json artifacts = receipt.value("artifacts", Json::array());
    if (!artifacts.is_array()
        || artifacts.size() != requiredDependencies.size() + 1u) {
        reject("skeletonBakeDependencyClosureInvalid",
            "Bake artifact table does not exactly cover the source closure.");
        return result;
    }
    std::unordered_set<std::string> artifactOutputs;
    bool hasSkeletonArtifact = false;
    for (const Json& artifact : artifacts) {
        if (!artifact.is_object()) {
            reject("skeletonBakeArtifactInvalid", "Bake artifact entry is invalid.");
            return result;
        }
        const std::string kind = artifact.value("kind", std::string{});
        const fs::path source = fs::absolute(fs::path(
            artifact.value("source", std::string{}))).lexically_normal();
        const fs::path output = fs::absolute(fs::path(
            artifact.value("output", std::string{}))).lexically_normal();
        const std::string sourceRevision = artifact.value(
            "sourceFingerprint", std::string{});
        const std::string normalizedOutput = portable(output);
        if (sourceRevision.empty()
            || sourceRevision != skeletonSourceFingerprint(source)
            || !outputPaths.contains(normalizedOutput)
            || !artifactOutputs.insert(normalizedOutput).second) {
            reject("skeletonBakeArtifactStale",
                "Bake artifact is stale, duplicated, or not listed as an output: "
                    + normalizedOutput);
            return result;
        }
        if (kind == "skeleton") {
            if (hasSkeletonArtifact
                || fs::weakly_canonical(source)
                    != fs::weakly_canonical(skeletonPath)
                || !isBakedSkeletonPath(output)) {
                reject("skeletonBakeArtifactInvalid",
                    "Bake receipt has an invalid skeleton artifact.");
                return result;
            }
            hasSkeletonArtifact = true;
            continue;
        }
        const std::string key = kind + "|" + portable(source);
        const auto required = requiredDependencies.find(key);
        const std::string extension = lower(output.extension().string());
        const bool extensionMatches =
            (kind == "animation" && ayt::asset_format::isCanonicalPath(output.string(), ayt::asset_format::Id::Animation))
            || (kind == "mesh" && ayt::asset_format::isCanonicalPath(output.string(), ayt::asset_format::Id::Mesh))
            || (kind == "skeletonMask" && ayt::asset_format::isCanonicalPath(output.string(), ayt::asset_format::Id::SkeletonMask));
        if (required == requiredDependencies.end() || !extensionMatches
            || !isBakedSkeletonArtifact(output)) {
            reject("skeletonBakeArtifactInvalid",
                "Bake artifact does not match a required dependency: "
                    + normalizedOutput);
            return result;
        }
        requiredDependencies.erase(required);
    }
    if (!hasSkeletonArtifact || !requiredDependencies.empty()
        || artifactOutputs.size() != outputPaths.size()) {
        reject("skeletonBakeDependencyClosureInvalid",
            "Bake artifact table has missing or extra closure members.");
        return result;
    }

    result.excludedPaths.insert(
        portable(fs::absolute(skeletonPath).lexically_normal()));
    result.excludedPaths.insert(
        portable(fs::absolute(mappingPath).lexically_normal()));
    result.excludedPaths.insert(
        portable(fs::absolute(receiptPath).lexically_normal()));
    fs::path dryRunPath = mappingPath;
    dryRunPath += ".bake-plan.json";
    result.excludedPaths.insert(
        portable(fs::absolute(dryRunPath).lexically_normal()));
    result.valid = true;
    diagnostic(diagnostics, ProjectBuildDiagnostic::Severity::Info, logical,
        "[skeletonBakeReady] Packaging will use cleaned baked skeleton outputs; authoring resources are excluded.");
    return result;
}

void diagnostic(std::vector<ProjectBuildDiagnostic>& target,
                ProjectBuildDiagnostic::Severity severity,
                std::string asset, std::string message)
{
    target.push_back({severity, std::move(asset), std::move(message)});
}

std::string hashFiles(const std::vector<fs::path>& paths,
                      const std::string& discriminator)
{
    auto hash = ayt::crypto::createHash(ayt::crypto::HashAlgo::SHA256);
    if (!hash) return {};
    hash->init();
    const auto update = [&hash](const std::string& value) {
        hash->update(reinterpret_cast<const std::uint8_t*>(value.data()),
                     value.size());
        constexpr std::uint8_t separator = 0;
        hash->update(&separator, 1);
    };
    update(discriminator);
    std::array<char, 64 * 1024> buffer{};
    for (const fs::path& path : paths) {
        update(portable(path.filename()));
        std::ifstream input(path, std::ios::binary);
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0) {
                hash->update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                             static_cast<std::size_t>(count));
            }
        }
    }
    std::array<std::uint8_t, 64> bytes{};
    hash->final(bytes.data());
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash->getHashSize() * 2);
    for (std::size_t index = 0; index < hash->getHashSize(); ++index) {
        result.push_back(digits[(bytes[index] >> 4) & 0x0f]);
        result.push_back(digits[bytes[index] & 0x0f]);
    }
    return result;
}

bool audioContentSource(const std::string& source){return asset_format::matchesPath(source,asset_format::Id::AudioChartSource)||asset_format::matchesPath(source,asset_format::Id::AudioPerformanceSource);}
void validateAudioContent(const ProjectBuildPlan& plan,std::vector<ProjectBuildDiagnostic>& diagnostics){
    const fs::path root=fs::path(plan.projectRoot)/plan.profile.content.assetRoot;
    for(const auto& asset:plan.assets){if(asset.transform==ProjectAssetTransform::Exclude)continue;
        try{if(audioContentSource(asset.sourcePath)){
                if(asset.transform!=ProjectAssetTransform::Cook)throw std::runtime_error("audio authoring content requires Cook to .acb");
                const auto source=inspectAudioContentSource(asset.sourcePath,root);
                for(const auto& dependency:source.dependencies){if(!isUnder(root,dependency))continue;const auto found=std::find_if(plan.assets.begin(),plan.assets.end(),[&](const auto& item){return fs::weakly_canonical(item.sourcePath)==dependency;});if(found==plan.assets.end()||found->transform==ProjectAssetTransform::Exclude)throw std::runtime_error("missing or excluded audio dependency: "+portable(dependency));}
            }else if(asset_format::matchesPath(asset.sourcePath,asset_format::Id::AudioContent)){AudioContent content;if(!content.load(asset.sourcePath))throw std::runtime_error("invalid compiled audio bundle");}
        }catch(const std::exception& e){diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,e.what());}
    }
}
std::vector<fs::path> sourceClosure(const fs::path& source, const fs::path& assetRoot)
{
    std::vector<fs::path> paths{source};
    if(asset_format::matchesPath(source.string(),asset_format::Id::AudioChartSource)||asset_format::matchesPath(source.string(),asset_format::Id::AudioPerformanceSource)){
        try{const auto inspected=inspectAudioContentSource(source,assetRoot);paths.insert(paths.end(),inspected.dependencies.begin(),inspected.dependencies.end());std::sort(paths.begin(),paths.end());return paths;}catch(...){return {};}
    }
    const std::string extension = lower(source.extension().string());
#if defined(AY_RESOURCE_HAS_STATS)
    if(ayt::asset_format::matchesPath(source.string(), ayt::asset_format::Id::StatsSource)) {
        auto inspected=stats::inspectRecipeAssetSource(source,assetRoot);
        if(!inspected) return {};
        paths.push_back(inspected->targetDefinitions);
        if(!inspected->sourceDefinitions.empty()) paths.push_back(inspected->sourceDefinitions);
        paths.push_back(inspected->recipeBook);
        std::sort(paths.begin(),paths.end());
        paths.erase(std::unique(paths.begin(),paths.end()),paths.end());
        return paths;
    }
#endif
#if defined(AY_RESOURCE_HAS_PARTICLE)
    if(ayt::asset_format::matchesPath(source.string(), ayt::asset_format::Id::ParticleEffect)) {
        particle::EffectSource effect;
        if(!particle::inspectEffectSource(source,assetRoot,effect)) return {};
        for(const auto& texture:effect.textures) if(!texture.empty()) paths.push_back(texture);
        std::sort(paths.begin(),paths.end());
        paths.erase(std::unique(paths.begin(),paths.end()),paths.end());
        return paths;
    }
#endif
    if (extension != ".fbx" && extension != ".gltf" && extension != ".glb") {
        return paths;
    }
    // Scene formats may refer to external textures without declaring those
    // source paths in the cooked sidecar. Hash sibling files conservatively:
    // this can cause an extra miss, but cannot return stale cooked output.
    std::error_code error;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(source.parent_path(), error)) {
        if (!error && entry.is_regular_file() && entry.path() != source) {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string cacheDiscriminator(const ProjectBuildProfile& profile,
                               const ProjectBuildAsset& asset)
{
    Json value = {
        {"contract", 1},
        {"platform", profile.platform},
        {"architecture", profile.architecture},
        {"configuration", profile.configuration},
        {"logicalPath", asset.logicalPath},
        {"transform", transformName(asset.transform)},
        {"cookTextures", asset.cookTextures},
        {"fbxImporterContract", std::string(kFbxImporterContractTag)},
    };
    if(asset_format::matchesPath(asset.sourcePath,asset_format::Id::AudioChartSource)||asset_format::matchesPath(asset.sourcePath,asset_format::Id::AudioPerformanceSource))value["audioContentImporterContract"]="audio-content-bundle-v1";
    if(ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::ParticleEffect))
        value["particleImporterContract"]="particle-closure-rgba8-v3";
#if defined(AY_RESOURCE_HAS_STATS)
    if(ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::StatsSource))
        value["statsImporterContract"]="stats-recipe-asset-v1";
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
    if(asset_format::matchesPath(asset.sourcePath,asset_format::Id::DeterministicLevelSource))
        value["detLevelImporterContract"]="deterministic-level-cooked-v1";
#endif
    return value.dump();
}

std::string computeCacheKey(const ProjectBuildProfile& profile,
                            const ProjectBuildAsset& asset)
{
    auto root=fs::path(asset.sourcePath);
    for(const auto& part:fs::path(asset.logicalPath)) { (void)part; root=root.parent_path(); }
    const auto closure=sourceClosure(asset.sourcePath,root);
    if(closure.empty()) return {};
    return hashFiles(closure,
                     cacheDiscriminator(profile, asset));
}

#if defined(AY_RESOURCE_HAS_DET_LEVEL)
bool validateDetLevelCookedFile(const fs::path& path,std::string& error,entity::DetLevel2D* output=nullptr) {
    std::error_code ec;
    const auto status=fs::symlink_status(path,ec);
    if(ec || !fs::is_regular_file(status)) {error="Cooked level must be an ordinary file";return false;}
    const auto size=fs::file_size(path,ec);
    if(ec || size<entity::kDetLevelCooked2DHeaderBytes || size>entity::kDetLevelCooked2DMaxBytes) {
        error="Cooked level exceeds the bounded envelope";return false;
    }
    std::ifstream stream(path,std::ios::binary);
    if(!stream) {error="Cannot open cooked level";return false;}
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(stream.gcount()!=static_cast<std::streamsize>(bytes.size()) || stream.peek()!=std::char_traits<char>::eof() || stream.bad()) {
        error="Cooked level changed size or could not be read completely";return false;
    }
    entity::DetLevel2D level;
    if(!entity::decodeDetLevelCooked2D(bytes,level,error))return false;
    if(output)*output=std::move(level);return true;
}
#endif

#if defined(AY_RESOURCE_HAS_DET_LEVEL)
void validateDetLevelContent(const ProjectBuildPlan& plan,
                            std::vector<ProjectBuildDiagnostic>& diagnostics) {
    for(const auto& asset:plan.assets) {
        if(asset.transform==ProjectAssetTransform::Exclude)continue;
        const auto source=asset_format::matchesPath(asset.sourcePath,asset_format::Id::DeterministicLevelSource);
        const auto cooked=asset_format::matchesPath(asset.sourcePath,asset_format::Id::DeterministicLevelCooked);
        if(!source && !cooked)continue;
        std::string error;
        try {
            if(source) {
                entity::DetLevel2D level;
                std::error_code ec;
                if(!fs::is_regular_file(fs::symlink_status(asset.sourcePath,ec)) || ec)
                    error="Deterministic level source must be an ordinary file";
                else if(asset.transform!=ProjectAssetTransform::Cook)
                    error="Deterministic level authoring requires Cook to .dlc";
                else if(entity::loadDetLevel2D(asset.sourcePath,level,error))
                    (void)entity::encodeDetLevelCooked2D(level);
            } else {
                (void)validateDetLevelCookedFile(asset.sourcePath,error);
            }
        } catch(const std::exception& exception) {error=exception.what();}
        if(!error.empty())diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,error);
    }
}
#endif

#if defined(AY_RESOURCE_HAS_STATS)
void validateStatsContent(const ProjectBuildPlan& plan,
                          std::vector<ProjectBuildDiagnostic>& diagnostics) {
    try {
        const auto root=fs::canonical(fs::path(plan.projectRoot)/plan.profile.content.assetRoot);
        std::map<fs::path,const ProjectBuildAsset*> entries;
        for(const auto& asset:plan.assets) entries.emplace(fs::weakly_canonical(asset.sourcePath),&asset);
        std::set<std::string> available;
        for(const auto& asset:plan.assets) {
            if(asset.transform==ProjectAssetTransform::Exclude) continue;
            if(ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::StatsSource) && asset.transform==ProjectAssetTransform::Cook) {
                const auto matched = ayt::asset_format::matchPath(asset.logicalPath);
                if (matched) available.insert(ayt::asset_format::appendSuffix(
                    std::string_view(asset.logicalPath).substr(0, asset.logicalPath.size() - matched->suffix.size()),
                    ayt::asset_format::Id::StatsRecipes));
            } else if(ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::StatsRecipes)) {
                available.insert(asset.logicalPath);
            }
        }
        for(const auto& asset:plan.assets) {
            if(asset.transform!=ProjectAssetTransform::Exclude && ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::StatsRecipes)) {
                auto decoded=stats::decodeRecipeAsset(io::File::readAllText(asset.sourcePath));
                if(!decoded)
                    diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,
                               "Invalid cooked stats asset: "+decoded.error().message);
            }
            if(asset.transform!=ProjectAssetTransform::Exclude && ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::Scene)) {
                Json scene=Json::parse(io::File::readAllText(asset.sourcePath),nullptr,false);
                if(!scene.is_discarded()) {
                    auto inspect=[&](auto&& self,const Json& node)->void {
                        if(node.is_object()) {
                            const auto type=node.find("$type");
                            if(type!=node.end() && *type=="StatsRecipeComponent") {
                                const auto path=node.find("assetPath");
                                if(path!=node.end() && path->is_string() && !path->get<std::string>().empty()) {
                                    const auto reference=path->get<std::string>();
                                    if(!portableRelative(reference) || !ayt::asset_format::matchesPath(reference, ayt::asset_format::Id::StatsRecipes) || !available.contains(fs::path(reference).generic_string()))
                                        diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,
                                                   "Missing or invalid StatsRecipeComponent asset: "+reference);
                                }
                                if(asset.storage==ProjectAssetStorage::Pak)
                                    diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,
                                               "Stats recipe scenes must be loose: SceneSerializer reads scene files from disk.");
                            }
                            for(const auto& item:node.items())self(self,item.value());
                        } else if(node.is_array())for(const auto& item:node)self(self,item);
                    };
                    inspect(inspect,scene);
                }
            }
            if(asset.transform==ProjectAssetTransform::Exclude || !ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::StatsSource)) continue;
            auto source=stats::inspectRecipeAssetSource(asset.sourcePath,root);
            if(!source) {
                diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,source.error().message);
                continue;
            }
            if(asset.transform!=ProjectAssetTransform::Cook) {
                diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,
                           "Stats source manifests must be cooked to .sts.");
            }
            for(const auto& path:{source->targetDefinitions,source->sourceDefinitions,source->recipeBook}) {
                if(path.empty()) continue;
                auto found=entries.find(path);
                if(found==entries.end() || found->second->transform==ProjectAssetTransform::Exclude)
                    diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,
                               "Missing or excluded stats dependency: "+path.generic_string());
            }
        }
    } catch(const std::exception& error) {
        diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,"stats content",error.what());
    }
}
#endif

#if defined(AY_RESOURCE_HAS_PARTICLE)
// Typed effects are checked before any publication. Scene inspection only
// traverses the two particle component contracts; unrelated schemas stay opaque.
void validateParticleContent(const ProjectBuildPlan& plan,
                             std::vector<ProjectBuildDiagnostic>& diagnostics) {
    try {
    const auto root=fs::canonical(fs::path(plan.projectRoot)/plan.profile.content.assetRoot);
    std::map<fs::path,const ProjectBuildAsset*> entries;
    for(const auto& asset:plan.assets) entries.emplace(fs::weakly_canonical(asset.sourcePath),&asset);
    auto fail=[&](const ProjectBuildAsset& asset,const std::string& message) {
        diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,message);
    };
    auto reference=[&](const ProjectBuildAsset& owner,const std::string& value)->const ProjectBuildAsset* {
        if(value.empty()) return nullptr;
        auto text=value; std::replace(text.begin(),text.end(),'\\','/');
        fs::path ref(text);
        if(ref.is_absolute() || ref.has_root_name() || text.find(':')!=std::string::npos) {
            fail(owner,"Particle reference must be asset-relative: "+value); return nullptr;
        }
        auto target=root/ref;
        if(!fs::is_regular_file(target)) target=fs::path(owner.sourcePath).parent_path()/ref;
        if(!fs::is_regular_file(target)) {
            fail(owner,"Missing particle dependency: "+value); return nullptr;
        }
        auto found=entries.find(fs::weakly_canonical(target));
        if(found==entries.end() || found->second->transform==ProjectAssetTransform::Exclude) {
            fail(owner,"Missing or excluded particle dependency: "+value); return nullptr;
        }
        return found->second;
    };
    for(const auto& asset:plan.assets) {
        if(asset.transform==ProjectAssetTransform::Exclude) continue;
        const auto extension=lower(fs::path(asset.sourcePath).extension().string());
        if(ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::ParticleEffect)) {
            particle::EffectSource effect; std::string error;
            if(!particle::inspectEffectSource(asset.sourcePath,root,effect,&error)) { fail(asset,error); continue; }
            for(size_t index=0; index<effect.textures.size(); ++index) {
                if(effect.textures[index].empty()) continue;
                const auto found=entries.find(effect.textures[index]);
                if(found==entries.end() || found->second->transform==ProjectAssetTransform::Exclude) {
                    fail(asset,"Missing or excluded particle dependency: "+effect.asset.emitters[index].effect.texturePath); continue;
                }
                const auto* texture=found->second;
                if(texture && asset.transform==ProjectAssetTransform::Raw && texture->transform==ProjectAssetTransform::Cook)
                    fail(asset,"Raw particle references a transformed texture; cook the particle effect too.");
                if(asset.transform==ProjectAssetTransform::Raw && texture->storage==ProjectAssetStorage::Pak) {
                    auto authored=effect.asset.emitters[index].effect.texturePath;
                    std::replace(authored.begin(),authored.end(),'\\','/');
                    if(fs::path(authored).lexically_normal().generic_string()!=texture->logicalPath)
                        fail(asset,"Cook owner-relative particle references before publishing their textures to Pak.");
                }
            }
        } else if(ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::Scene)) {
            Json scene=Json::parse(io::File::readAllText(asset.sourcePath),nullptr,false);
            if(scene.is_discarded()) continue; // The scene validator owns unrelated malformed scenes.
            auto inspect=[&](auto&& self,const Json& node)->void {
                if(node.is_object()) {
                    const auto type=node.find("$type");
                    if(type!=node.end() && type->is_string()) {
                        const bool effect=*type=="ParticleEffectComponent";
                        const bool emitter=*type=="ParticleEmitterComponent";
                        if(effect || emitter) {
                            const auto field=node.find(effect?"effectPath":"texturePath");
                            if(field!=node.end() && field->is_string() && !field->get<std::string>().empty()) {
                                const auto* dependency=reference(asset,field->get<std::string>());
                                if(effect && dependency && !ayt::asset_format::matchesPath(dependency->sourcePath, ayt::asset_format::Id::ParticleEffect))
                                    fail(asset,"ParticleEffectComponent must reference a .pfx asset.");
                                if(emitter && dependency && dependency->transform==ProjectAssetTransform::Cook)
                                    fail(asset,"Inline particle texture changes path during cook; use a reusable .pfx effect or final .txr.");
                            }
                            if(asset.storage==ProjectAssetStorage::Pak)
                                fail(asset,"Particle showcase scenes must be loose: SceneSerializer reads scene files from disk.");
                        }
                    }
                    for(const auto& item:node.items()) self(self,item.value());
                } else if(node.is_array()) for(const auto& item:node) self(self,item);
            };
            inspect(inspect,scene);
        }
    }
    } catch(const std::exception& error) {
        diagnostic(diagnostics,ProjectBuildDiagnostic::Severity::Error,"particle content",error.what());
    }
}
#endif

void validateActorContent(const ProjectBuildPlan& plan,
                          std::vector<ProjectBuildDiagnostic>& diagnostics)
{
    const fs::path root = fs::path(plan.projectRoot) / plan.profile.content.assetRoot;
    std::unordered_map<std::string, const ProjectBuildAsset*> entries;
    for (const auto& asset : plan.assets) entries.emplace(asset.logicalPath, &asset);
    std::unordered_map<std::string, std::string> parents;
    auto fail = [&](const ProjectBuildAsset& owner, const std::string& message) {
        diagnostic(diagnostics, ProjectBuildDiagnostic::Severity::Error,
                   owner.logicalPath, message);
    };
    auto reference = [&](const ProjectBuildAsset& owner,
                         const std::string& path,
                         const std::string& extension) -> const ProjectBuildAsset* {
        const fs::path relative(path);
        if (!portableRelative(path) || path.find('\\') != std::string::npos
            || path.find(':') != std::string::npos
            || relative.lexically_normal().generic_string() != path
            || (extension == ".act"
                ? !ayt::asset_format::matchesPath(path, ayt::asset_format::Id::ActorClass)
                : lower(relative.extension().string()) != extension)
            || !isUnder(root, root / relative)) {
            fail(owner, "Invalid Actor reference: " + path);
            return nullptr;
        }
        const auto found = entries.find(path);
        if (found == entries.end() || found->second->transform == ProjectAssetTransform::Exclude) {
            fail(owner, "Missing or excluded Actor dependency: " + path);
            return nullptr;
        }
        const auto* target = found->second;
        if (!fs::is_regular_file(target->sourcePath)) {
            fail(owner, "Actor dependency disappeared after planning: " + path);
            return nullptr;
        }
        if (target->transform != ProjectAssetTransform::Raw
            || target->storage != ProjectAssetStorage::Loose) {
            fail(owner, "Actor dependency must be raw and loose: " + path);
            return nullptr;
        }
        return target;
    };
    for (const auto& asset : plan.assets) {
        if (asset.transform == ProjectAssetTransform::Exclude) continue;
        const bool actorClass = ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::ActorClass);
        if (!actorClass && !ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::Scene)) continue;
        try {
            if (actorClass) {
                if (asset.transform != ProjectAssetTransform::Raw
                    || asset.storage != ProjectAssetStorage::Loose) {
                    fail(asset, "Actor class must be raw and loose for runtime loading.");
                }
                std::error_code sizeError;
                if (fs::file_size(asset.sourcePath, sizeError) > 1024u * 1024u || sizeError) {
                    fail(asset, "Actor class exceeds 1 MiB or is unreadable.");
                    continue;
                }
                const Json doc = Json::parse(io::File::readAllText(asset.sourcePath));
                const int actorSchema = doc.value("schemaVersion", 0);
                if (!doc.is_object() || doc.value("type", std::string{}) != "ay.actorClass"
                    || (actorSchema != 1 && actorSchema != 2 && actorSchema != 3)
                    || !doc.contains("id") || !doc["id"].is_string()) {
                    fail(asset, "Invalid Actor class type, id or schema.");
                    continue;
                }
                const auto script = doc.value("script", std::string{});
                if (!script.empty()) reference(asset, script, ".logia");
                if (actorSchema >= 2) {
                    const auto parent = doc.value("parent", std::string{});
                    if (!parent.empty() && reference(asset, parent, ".act"))
                        parents[asset.logicalPath] = parent;
                }
                const Json properties = doc.value("properties", Json::object());
                if (!properties.is_object()) fail(asset, "Actor properties must be an object.");
                else for (auto it = properties.begin(); it != properties.end(); ++it) {
                    if (!(it.value().is_boolean() || it.value().is_number()
                          || it.value().is_string())) {
                        fail(asset, "Actor property must be primitive: " + it.key());
                    }
                }
                const Json components = doc.value("components", Json::array());
                if (!components.is_array()) fail(asset, "Actor components must be an array.");
                else {
                    std::unordered_set<std::string> seen;
                    for (const Json& item : components) {
                        if (!item.is_object() || !item.contains("$type")
                            || !item["$type"].is_string()) {
                            fail(asset, "Actor component has missing $type.");
                            continue;
                        }
                        const std::string type = item["$type"].get<std::string>();
                        if (actorSchema < 3 && !seen.insert(type).second) {
                            fail(asset, "Legacy Actor component has duplicate $type.");
                        } else if (actorSchema == 3) {
                            const std::string id = item.value("$instanceId", std::string{});
                            const bool validId = id.size() == 32
                                && std::all_of(id.begin(), id.end(), [](unsigned char c) {
                                    return (c >= '0' && c <= '9')
                                        || (c >= 'a' && c <= 'f');
                                });
                            if (!validId || !seen.insert(id).second) {
                                fail(asset, "Actor component has missing or duplicate $instanceId.");
                            }
                        }
                    }
                }
            } else {
                const Json scene = Json::parse(io::File::readAllText(asset.sourcePath),
                                               nullptr, false);
                if (scene.is_discarded()) continue; // Scene validation owns syntax errors.
                bool hasActor = false;
                auto inspect = [&](auto&& self, const Json& node) -> void {
                    if (node.is_object()) {
                        const auto type = node.find("$type");
                        if (type != node.end() && *type == "ActorInstanceComponent") {
                            hasActor = true;
                            const auto path = node.find("classPath");
                            if (path == node.end() || !path->is_string()
                                || path->get<std::string>().empty()) {
                                fail(asset, "Actor instance has no classPath.");
                            } else reference(asset, path->get<std::string>(), ".act");
                        }
                        for (const auto& item : node.items()) self(self, item.value());
                    } else if (node.is_array()) {
                        for (const auto& item : node) self(self, item);
                    }
                };
                inspect(inspect, scene);
                if (hasActor && (asset.transform != ProjectAssetTransform::Raw
                                 || asset.storage != ProjectAssetStorage::Loose)) {
                    fail(asset, "Scene with Actor instances must be raw and loose.");
                }
            }
        } catch (const std::exception& ex) {
            fail(asset, std::string("Invalid Actor content: ") + ex.what());
        }
    }
    std::unordered_set<std::string> active;
    std::unordered_set<std::string> done;
    std::function<void(const std::string&)> checkCycle = [&](const std::string& path) {
        if (done.contains(path)) return;
        if (!active.insert(path).second) {
            if (const auto found = entries.find(path); found != entries.end())
                fail(*found->second, "Actor inheritance cycle: " + path);
            return;
        }
        if (const auto found = parents.find(path); found != parents.end())
            checkCycle(found->second);
        active.erase(path);
        done.insert(path);
    };
    for (const auto& entry : parents) checkCycle(entry.first);
}

ayt::storage::CompressionAlgo compressionOf(const std::string& value)
{
    const std::string normalized = lower(value);
    if (normalized == "none") return ayt::storage::CompressionAlgo::None;
    if (normalized == "lz4") return ayt::storage::CompressionAlgo::Lz4;
    return ayt::storage::CompressionAlgo::Zstd;
}

ayt::platform::ProcessResult runProcess(const std::vector<std::string>& arguments,
               const fs::path& workingDirectory)
{
    if (arguments.empty()) { ayt::platform::ProcessResult failed; failed.error = "Empty process command"; return failed; }
    ayt::platform::ProcessOptions options;
    options.executable = fs::u8path(arguments.front());
    options.arguments.assign(arguments.begin() + 1, arguments.end());
    options.workingDirectory = workingDirectory;
    options.captureOutput = false;
    return ayt::platform::runProcess(options);
}

void report(const ProjectBuildProgressFn& progress, ProjectBuildStage stage,
            float fraction, std::string message)
{
    if (progress) progress({stage, fraction, std::move(message)});
}

bool copyFile(const fs::path& source, const fs::path& destination,
              std::string& error)
{
    std::error_code fileError;
    const auto nativeSource = nativeFilesystemPath(source);
    const auto nativeDestination = nativeFilesystemPath(destination);
    fs::create_directories(nativeDestination.parent_path(), fileError);
    if (fileError) {
        error = "Could not create staging directory: " + fileError.message();
        return false;
    }
    fs::copy_file(nativeSource, nativeDestination,
                  fs::copy_options::overwrite_existing, fileError);
    if (fileError) {
        error = "Could not stage '" + source.string() + "': "
            + fileError.message();
        return false;
    }
    return true;
}

bool isRuntimeLibrary(const fs::path& path, std::string_view platform)
{
    const std::string filename = lower(path.filename().string());
    const std::string extension = lower(path.extension().string());
    const std::string target = lower(std::string(platform));
    if (target == "windows") return extension == ".dll";
    if (target == "macos" || target == "mac" || target == "darwin") {
        return extension == ".dylib";
    }
    if (target == "linux") {
        return filename.ends_with(".so")
            || filename.find(".so.") != std::string::npos;
    }
    return false;
}

bool stageAdjacentRuntimeLibraries(const fs::path& artifact,
                                   std::string_view platform,
                                   const fs::path& staging,
                                   std::vector<std::string>& stagedNames,
                                   std::string& error)
{
    std::error_code fileError;
    std::vector<fs::path> libraries;
    for (fs::directory_iterator it(artifact.parent_path(), fileError), end;
         it != end && !fileError; it.increment(fileError)) {
        if (it->is_regular_file(fileError)
            && !fileError
            && isRuntimeLibrary(it->path(), platform)) {
            libraries.push_back(it->path());
        }
    }
    if (fileError) {
        error = "Could not inspect runtime dependencies next to '"
            + artifact.string() + "': " + fileError.message();
        return false;
    }
    std::sort(libraries.begin(), libraries.end(),
        [](const fs::path& left, const fs::path& right) {
            return left.filename().generic_string()
                < right.filename().generic_string();
        });
    for (const fs::path& library : libraries) {
        const std::string filename = library.filename().generic_string();
        if (!copyFile(library, staging / library.filename(), error)) {
            return false;
        }
        stagedNames.push_back(filename);
    }
    return true;
}

struct StagedFile {
    fs::path diskPath;
    std::string logicalPath;
    ProjectAssetStorage storage = ProjectAssetStorage::Loose;
    std::string chunk;
    std::string type;
    std::vector<ConversionResult::Dependency> dependencies;
    bool raw = false;
    std::string source;
    std::string cacheKey;
    std::vector<std::string> declaredResources;
};

bool loadCachedConversion(const fs::path& objectRoot,
                          ConversionResult& conversion)
{
    const fs::path metadata = objectRoot / "conversion.json";
    if (!fs::is_regular_file(metadata)) return false;
    const std::string text = ayt::io::File::readAllText(metadata.string());
    if (text.empty()) return false;
    try {
        conversion = ConversionResult::fromJson(text);
    } catch (...) {
        return false;
    }
    if (conversion.resources.empty()) return false;
    for (const auto& resource : conversion.resources) {
        if (!fs::is_regular_file(nativeFilesystemPath(objectRoot / "files" / resource.path))) {
            return false;
        }
    }
    return true;
}

bool publishCacheObject(const fs::path& temporary,
                        const fs::path& objectRoot,
                        const ConversionResult& conversion,
                        std::string& error)
{
    const std::string encoded = conversion.toJson();
    if (!ayt::io::File::atomicWrite(
            (temporary / "conversion.json").string(),
            encoded.data(), encoded.size())) {
        error = "Could not write cook cache metadata.";
        return false;
    }
    std::error_code fileError;
    fs::create_directories(objectRoot.parent_path(), fileError);
    if (fileError) {
        error = "Could not create cook cache object directory: "
            + fileError.message();
        return false;
    }
    if (fs::exists(objectRoot)) fs::remove_all(objectRoot, fileError);
    if (fileError) {
        error = "Could not clear cook cache object: " + fileError.message();
        return false;
    }
    const auto publication = detail::retryPublishFileOperation(
        [&](std::error_code& ec) { fs::rename(temporary, objectRoot, ec); },
        [](std::size_t, const std::error_code&) {});
    if (publication.error) {
        error = detail::publishFileError("publish cook cache object", temporary, objectRoot, publication);
        return false;
    }
    return true;
}

ProjectCookPolicy effectivePolicy(const ProjectBuildProfile& profile,
                                  const ProjectBuildExecutionOptions& options)
{
    if (options.forceCook) return ProjectCookPolicy::Force;
    if (options.cacheOnly) return ProjectCookPolicy::CacheOnly;
    if (options.neverCook) return ProjectCookPolicy::Never;
    return profile.cache.policy;
}

bool insertResourceRecord(ayt::storage::IStorageDatabase& database,
                          const StagedFile& file, const std::string& package,
                          const std::string& contentHash)
{
    if (file.type.empty()) return true;
    ayt::storage::ResourceRecord record;
    record.path = file.logicalPath;
    record.type = file.type;
    record.format = fs::path(file.logicalPath).extension().string();
    std::error_code error;
    record.size = static_cast<std::int64_t>(fs::file_size(file.diskPath, error));
    if (error) record.size = 0;
    record.inPackage = package;
    record.hash = contentHash;
    return database.insertResource(record);
}

} // namespace

bool ProjectBuildPlan::valid() const noexcept
{
    return std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const ProjectBuildDiagnostic& item) {
            return item.severity == ProjectBuildDiagnostic::Severity::Error;
        });
}

std::string ProjectBuildPlan::serialize(bool pretty) const
{
    Json root = {
        {"format", "AYProjectBuildPlan"},
        {"version", 1},
        {"profile", profile.id},
        {"projectRoot", projectRoot},
        {"valid", valid()},
        {"assets", Json::array()},
        {"diagnostics", Json::array()},
    };
    for (const ProjectBuildAsset& asset : assets) {
        root["assets"].push_back({
            {"logicalPath", asset.logicalPath},
            {"transform", transformName(asset.transform)},
            {"storage", storageName(asset.storage)},
            {"chunk", asset.chunk},
            {"cacheKey", asset.cacheKey},
            {"scriptDependencies", asset.scriptDependencies},
            {"resourceDependencies", asset.resourceDependencies},
            {"matchedRule", asset.matchedRule == static_cast<std::size_t>(-1)
                ? Json(nullptr) : Json(asset.matchedRule)},
        });
    }
    for (const ProjectBuildDiagnostic& item : diagnostics) {
        const char* severity = item.severity
                == ProjectBuildDiagnostic::Severity::Error ? "error"
            : item.severity == ProjectBuildDiagnostic::Severity::Warning
                ? "warning" : "info";
        root["diagnostics"].push_back({
            {"severity", severity}, {"asset", item.asset},
            {"message", item.message},
        });
    }
    return root.dump(pretty ? 2 : -1);
}

ProjectBuildPlan ProjectBuildPlanner::create(
    const ProjectBuildProfile& profile, const std::string& projectRoot)
{
    ProjectBuildPlan plan;
    plan.profile = profile;
    std::error_code error;
    const fs::path root = fs::weakly_canonical(
        fs::absolute(projectRoot.empty() ? fs::current_path()
                                        : fs::path(projectRoot)), error);
    plan.projectRoot = (error ? fs::absolute(projectRoot) : root).string();
    std::string validation;
    if (!profile.validate(&validation)) {
        diagnostic(plan.diagnostics, ProjectBuildDiagnostic::Severity::Error,
                   {}, std::move(validation));
        return plan;
    }
    const fs::path assetsRoot = root / profile.content.assetRoot;
    if (!fs::is_directory(assetsRoot)) {
        diagnostic(plan.diagnostics, ProjectBuildDiagnostic::Severity::Error,
                   profile.content.assetRoot, "Asset root is not a directory.");
        return plan;
    }
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(assetsRoot,
             fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error)) files.push_back(it->path());
    }
    if (error) {
        diagnostic(plan.diagnostics, ProjectBuildDiagnostic::Severity::Error,
                   profile.content.assetRoot,
                   "Asset scan failed: " + error.message());
        return plan;
    }
    std::sort(files.begin(), files.end());
    std::unordered_set<std::string> skeletonAuthoringExclusions;
    std::unordered_set<std::string> skeletonBakeOutputs;
    for (const fs::path& file : files) {
        if (!ayt::asset_format::matchesPath(file.string(), ayt::asset_format::Id::Skeleton)
            || isBakedSkeletonPath(file)) {
            continue;
        }
        SkeletonReleaseGateResult gate = validateSkeletonReleaseGate(
            file, assetsRoot, plan.diagnostics);
        skeletonAuthoringExclusions.insert(
            gate.excludedPaths.begin(), gate.excludedPaths.end());
        skeletonBakeOutputs.insert(
            gate.allowedPaths.begin(), gate.allowedPaths.end());
    }
    for (const fs::path& file : files) {
        ProjectBuildAsset asset;
        asset.sourcePath = file.string();
        asset.logicalPath = portable(fs::relative(file, assetsRoot));
        asset.transform = profile.content.defaultTransform;
        asset.storage = profile.content.defaultStorage;
        asset.chunk = "core";
        for (std::size_t index = 0; index < profile.content.rules.size(); ++index) {
            const ProjectBuildRule& rule = profile.content.rules[index];
            if (!matches(rule.match, asset.logicalPath)) continue;
            asset.transform = rule.transform;
            asset.storage = rule.storage;
            asset.chunk = rule.chunk;
            asset.cookTextures = rule.cookTextures;
            asset.matchedRule = index;
            break;
        }
        if (fs::path(asset.logicalPath).extension() == ".logia"
            && asset.matchedRule == static_cast<std::size_t>(-1)
            && asset.transform != ProjectAssetTransform::Exclude) {
            asset.transform = ProjectAssetTransform::Raw;
            asset.storage = ProjectAssetStorage::Loose;
        }
        if (asset.transform == ProjectAssetTransform::Auto) {
            asset.transform = isImportSupportedExtension(asset.sourcePath)
                ? ProjectAssetTransform::Cook
                : ProjectAssetTransform::Raw;
        }
        const std::string absoluteFile = portable(
            fs::absolute(file).lexically_normal());
        if (isSkeletonAuthoringMetadata(file)
            || (isBakedSkeletonArtifact(file)
                && !skeletonBakeOutputs.contains(absoluteFile))
            || skeletonAuthoringExclusions.contains(absoluteFile)) {
            asset.transform = ProjectAssetTransform::Exclude;
            asset.cacheKey.clear();
        }
        if (asset.transform == ProjectAssetTransform::Cook
            && !isImportSupportedExtension(asset.sourcePath)) {
            diagnostic(plan.diagnostics,
                       ProjectBuildDiagnostic::Severity::Error,
                       asset.logicalPath,
                       "No registered importer can cook this source extension.");
        }
        if (asset.storage == ProjectAssetStorage::Pak && asset.chunk.empty()) {
            asset.chunk = "core";
        }
        if (asset.transform == ProjectAssetTransform::Cook) {
            asset.cacheKey = computeCacheKey(profile, asset);
            if (asset.cacheKey.empty()) {
                diagnostic(plan.diagnostics,
                           ProjectBuildDiagnostic::Severity::Error,
                           asset.logicalPath,
                           "Could not compute the content-addressed cook key.");
            }
        }
        if(ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::PhoskiaShader)
            && asset.transform!=ProjectAssetTransform::Exclude
            && (asset.transform!=ProjectAssetTransform::Raw || asset.storage!=ProjectAssetStorage::Loose)) {
            diagnostic(plan.diagnostics,ProjectBuildDiagnostic::Severity::Error,asset.logicalPath,
                "Phoskia programs/modules require raw/loose publication for portable source lookup.");
        }
        plan.assets.push_back(std::move(asset));
    }
#if defined(AY_RESOURCE_HAS_PARTICLE)
    validateParticleContent(plan,plan.diagnostics);
#endif
#if defined(AY_RESOURCE_HAS_STATS)
    validateStatsContent(plan,plan.diagnostics);
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
    validateDetLevelContent(plan,plan.diagnostics);
#endif
    validateAudioContent(plan,plan.diagnostics);
    validateActorContent(plan, plan.diagnostics);
    detail::discoverProjectLogia(plan);
    return plan;
}

ProjectBuildResult ProjectBuildExecutor::execute(
    const ProjectBuildPlan& plan,
    const ProjectBuildExecutionOptions& options,
    ProjectBuildProgressFn progress)
{
    ProjectBuildResult result;
    result.diagnostics = plan.diagnostics;
    report(progress, ProjectBuildStage::Validate, 0.0f, "validating plan");
    if (!plan.valid()) {
        result.error = "Project build plan contains errors.";
        return result;
    }
    const fs::path releaseAssetsRoot = fs::path(plan.projectRoot)
        / plan.profile.content.assetRoot;
    std::vector<ProjectBuildDiagnostic> releaseDiagnostics;
    validateAudioContent(plan,releaseDiagnostics);
    for(const auto& asset:plan.assets)if(asset.transform==ProjectAssetTransform::Cook&&audioContentSource(asset.sourcePath)&&asset.cacheKey!=computeCacheKey(plan.profile,asset)){
        result.error="Audio content inputs changed after planning; create a fresh build plan.";return result;
    }
    validateActorContent(plan, releaseDiagnostics);
    detail::validateProjectLogia(plan, releaseDiagnostics);
    if (std::any_of(releaseDiagnostics.begin(), releaseDiagnostics.end(),
            [](const auto& item) {
                return item.severity == ProjectBuildDiagnostic::Severity::Error;
            })) {
        result.diagnostics.insert(result.diagnostics.end(),
            releaseDiagnostics.begin(), releaseDiagnostics.end());
        result.error = "Actor content or Logia import validation rejected the project build.";
        return result;
    }
#if defined(AY_RESOURCE_HAS_PARTICLE)
    validateParticleContent(plan,releaseDiagnostics);
    if(std::any_of(releaseDiagnostics.begin(),releaseDiagnostics.end(),[](const auto& item) {
        return item.severity==ProjectBuildDiagnostic::Severity::Error;
    })) {
        result.diagnostics.insert(result.diagnostics.end(),releaseDiagnostics.begin(),releaseDiagnostics.end());
        result.error="Particle content validation rejected the project build.";
        return result;
    }
    for(const auto& asset:plan.assets) {
        if(asset.transform==ProjectAssetTransform::Cook
           && ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::ParticleEffect)
           && asset.cacheKey!=computeCacheKey(plan.profile,asset)) {
            result.error="Particle inputs changed after planning; create a fresh build plan.";
            return result;
        }
    }
#endif
#if defined(AY_RESOURCE_HAS_STATS)
    validateStatsContent(plan,releaseDiagnostics);
    if(std::any_of(releaseDiagnostics.begin(),releaseDiagnostics.end(),[](const auto& item) {
        return item.severity==ProjectBuildDiagnostic::Severity::Error;
    })) {
        result.diagnostics.insert(result.diagnostics.end(),releaseDiagnostics.begin(),releaseDiagnostics.end());
        result.error="Stats content validation rejected the project build.";
        return result;
    }
    for(const auto& asset:plan.assets) {
        if(asset.transform==ProjectAssetTransform::Cook && ayt::asset_format::matchesPath(asset.sourcePath, ayt::asset_format::Id::StatsSource)
           && asset.cacheKey!=computeCacheKey(plan.profile,asset)) {
            result.error="Stats inputs changed after planning; create a fresh build plan.";
            return result;
        }
    }
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
    validateDetLevelContent(plan,releaseDiagnostics);
    if(std::any_of(releaseDiagnostics.begin(),releaseDiagnostics.end(),[](const auto& item) {
        return item.severity==ProjectBuildDiagnostic::Severity::Error;
    })) {
        result.diagnostics.insert(result.diagnostics.end(),releaseDiagnostics.begin(),releaseDiagnostics.end());
        result.error="Deterministic level content validation rejected the project build.";return result;
    }
    for(const auto& asset:plan.assets) {
        if(asset.transform==ProjectAssetTransform::Cook
           && asset_format::matchesPath(asset.sourcePath,asset_format::Id::DeterministicLevelSource)
           && asset.cacheKey!=computeCacheKey(plan.profile,asset)) {
            result.error="Deterministic level inputs changed after planning; create a fresh build plan.";return result;
        }
    }
#endif
    for (const ProjectBuildAsset& asset : plan.assets) {
        const fs::path source(asset.sourcePath);
        if (!ayt::asset_format::matchesPath(source.string(), ayt::asset_format::Id::Skeleton)
            || isBakedSkeletonPath(source)) {
            continue;
        }
        (void)validateSkeletonReleaseGate(
            source, releaseAssetsRoot, releaseDiagnostics);
    }
    result.diagnostics.insert(result.diagnostics.end(),
        releaseDiagnostics.begin(), releaseDiagnostics.end());
    if (std::any_of(releaseDiagnostics.begin(), releaseDiagnostics.end(),
            [](const ProjectBuildDiagnostic& item) {
                return item.severity == ProjectBuildDiagnostic::Severity::Error;
            })) {
        result.error = "Skeleton release gate rejected the project build.";
        return result;
    }
    const fs::path projectRoot = plan.projectRoot;
    const fs::path output = projectRoot / plan.profile.package.output;
    const fs::path staging = fs::path(output.string() + ".staging");
    const fs::path contentRoot = staging / plan.profile.content.outputSubdirectory;
    const fs::path cacheRoot = projectRoot / plan.profile.cache.root;
    if (!isUnder(projectRoot, output) || !isUnder(projectRoot, cacheRoot)
        || output == projectRoot || cacheRoot == projectRoot) {
        result.error = "Resolved package/cache path escapes the project root.";
        return result;
    }
    result.outputDirectory = output.string();
    if (options.dryRun) {
        result.ok = true;
        for (const ProjectBuildAsset& asset : plan.assets) {
            if (asset.transform == ProjectAssetTransform::Raw) ++result.rawCount;
            else if (asset.transform == ProjectAssetTransform::Cook) ++result.cookedCount;
            else if (asset.transform == ProjectAssetTransform::Exclude) ++result.excludedCount;
        }
        report(progress, ProjectBuildStage::Done, 1.0f, "dry run complete");
        return result;
    }

    std::error_code fileError;
    const auto clearStaging = detail::retryPublishFileOperation(
        [&](std::error_code& error) { fs::remove_all(staging, error); },
        [&](std::size_t attempt, const std::error_code&) {
            report(progress, ProjectBuildStage::Stage, 0.1f,
                "Retrying clear staging directory (attempt " + std::to_string(attempt) + ")");
        });
    if (clearStaging.error) {
        result.error = detail::publishFileError("clear staging directory", staging, {}, clearStaging);
        return result;
    }
    fs::create_directories(contentRoot, fileError);
    if (fileError) {
        result.error = "Could not create staging directory: " + fileError.message();
        return result;
    }

    fs::path builtArtifact;
    std::vector<std::string> runtimeFiles;
    if (plan.profile.code.enabled) {
        const fs::path sourceDirectory =
            projectRoot / plan.profile.code.sourceDirectory;
        if (!options.skipCode) {
            report(progress, ProjectBuildStage::ConfigureCode, 0.02f,
                   "configuring CMake project");
            const auto configure = runProcess({
                plan.profile.code.cmakeExecutable,
                "--preset", plan.profile.code.configurePreset,
                "-S", sourceDirectory.string(),
            }, projectRoot);
            if (configure.exitCode != 0 || !configure.error.empty()) {
                result.error = "CMake configure failed with exit code "
                    + std::to_string(configure.exitCode) + ". " + configure.error;
                return result;
            }
            report(progress, ProjectBuildStage::BuildCode, 0.08f,
                   "building CMake target " + plan.profile.code.target);
            const auto build = runProcess({
                plan.profile.code.cmakeExecutable,
                "--build", "--preset", plan.profile.code.buildPreset,
                "--target", plan.profile.code.target,
            }, projectRoot);
            if (build.exitCode != 0 || !build.error.empty()) {
                result.error = "CMake build failed with exit code "
                    + std::to_string(build.exitCode) + ". " + build.error;
                return result;
            }
        }
        builtArtifact = projectRoot / plan.profile.code.artifact;
        if (!fs::is_regular_file(builtArtifact)) {
            result.error = "Configured build artifact was not produced: "
                + builtArtifact.string();
            return result;
        }
        if (!copyFile(builtArtifact, staging / builtArtifact.filename(),
                      result.error)) {
            return result;
        }
        if (!stageAdjacentRuntimeLibraries(
                builtArtifact, plan.profile.platform, staging,
                runtimeFiles, result.error)) {
            return result;
        }
        result.executable = (output / builtArtifact.filename()).string();
    }

    const ProjectCookPolicy policy = effectivePolicy(plan.profile, options);
    std::vector<StagedFile> staged;
    staged.reserve(plan.assets.size());
    std::size_t processed = 0;
    for (const ProjectBuildAsset& asset : plan.assets) {
        const float fraction = plan.assets.empty() ? 0.8f
            : 0.1f + 0.65f * static_cast<float>(processed)
                / static_cast<float>(plan.assets.size());
        ++processed;
        if (asset.transform == ProjectAssetTransform::Exclude) {
            ++result.excludedCount;
            continue;
        }
        if (asset.transform == ProjectAssetTransform::Raw) {
            ++result.rawCount;
            staged.push_back({asset.sourcePath, asset.logicalPath,
                asset.storage, asset.chunk,
                ResourceRegistry::getTypeFromPath(asset.logicalPath), {},
                true, asset.logicalPath, {}});
            for (const auto& dependency : asset.scriptDependencies)
                staged.back().dependencies.push_back({asset.logicalPath, dependency});
            for (const auto& dependency : asset.resourceDependencies)
                staged.back().dependencies.push_back({asset.logicalPath, dependency});
            staged.back().declaredResources=asset.resourceDependencies;
            continue;
        }

        ++result.cookedCount;
        report(progress, ProjectBuildStage::Cook, fraction,
               "cook " + asset.logicalPath);
        if (!plan.profile.cache.enabled) {
            result.error = "Cook rules require cache.enabled=true so outputs "
                "can be published transactionally.";
            return result;
        }
        // Carry one native physical spelling through cache probe, metadata
        // reads, replacement/rename, hashing and Pak reads. Importer-facing
        // directory strings retain their existing ordinary-path contract.
        const fs::path objectRoot = nativeFilesystemPath(cacheRoot / "objects"
            / asset.cacheKey.substr(0, 2) / asset.cacheKey);
        ConversionResult conversion;
        const bool cacheHit = policy != ProjectCookPolicy::Force
            && loadCachedConversion(objectRoot, conversion);
        if (cacheHit) {
            ++result.cacheHitCount;
        } else {
            if (policy == ProjectCookPolicy::CacheOnly
                || policy == ProjectCookPolicy::Never) {
                result.error = "Cook cache miss for '" + asset.logicalPath
                    + "' while policy is " + policyName(policy) + ".";
                return result;
            }
            const fs::path temporaryPath = cacheRoot / "staging"
                / (asset.cacheKey + ".tmp");
            const fs::path temporary = nativeFilesystemPath(temporaryPath);
            if (fs::exists(temporary)) fs::remove_all(temporary, fileError);
            fileError.clear();
            fs::create_directories(temporary / "files", fileError);
            if (fileError) {
                result.error = "Could not create cook cache staging: "
                    + fileError.message();
                return result;
            }
            ImportOptions import;
            import.sourcePath = asset.sourcePath;
            // Existing converters append portable separators to this string;
            // do not expose Windows extended spelling through that interface.
            // The level converter reacquires native paths at each physical access.
            import.outputDir = (temporaryPath / "files").string();
            import.force = true;
            import.requireCharacterAssets = false;
            import.cookTextures = asset.cookTextures;
            import.sourceAssetRoot = releaseAssetsRoot.string();
            ImportResult imported = importAsset(import);
            if (!imported.ok || imported.conversion.resources.empty()) {
                result.error = "Cook failed for '" + asset.logicalPath + "': "
                    + (imported.error.empty() ? "no resources produced"
                                               : imported.error);
                return result;
            }
            conversion = std::move(imported.conversion);
            if (!publishCacheObject(temporary, objectRoot, conversion,
                                    result.error)) {
                return result;
            }
        }
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
        if(asset_format::matchesPath(asset.sourcePath,asset_format::Id::DeterministicLevelSource)) {
            auto expected=fs::path(asset.logicalPath);
            expected.replace_extension(asset_format::suffix(asset_format::Id::DeterministicLevelCooked));
            if(conversion.importerContractTag!="deterministic-level-cooked-v1"
               || conversion.resources.size()!=1 || !conversion.dependencies.empty()
               || conversion.resources.front().type!="DeterministicLevel2D"
               || conversion.resources.front().path!=expected.generic_string()) {
                result.error="Invalid deterministic level cached conversion identity: "+asset.logicalPath;return result;
            }
        }
#endif
        for (const auto& resource : conversion.resources) {
            if(audioContentSource(asset.sourcePath)){
                const auto matched=asset_format::matchPath(asset.logicalPath);const auto expected=asset_format::appendSuffix(std::string_view(asset.logicalPath).substr(0,asset.logicalPath.size()-matched->suffix.size()),asset_format::Id::AudioContent);
                if(conversion.resources.size()!=1||!conversion.dependencies.empty()||conversion.importerContractTag!="audio-content-bundle-v1"||resource.type!="AudioContent"){result.error="Invalid audio content cache identity: "+asset.logicalPath;return result;}
                if(resource.path!=expected){result.error="Audio content cache path mismatch: expected "+expected+", got "+resource.path;return result;}
                AudioContent content;if(!content.load((objectRoot/"files"/resource.path).string())){result.error="Corrupt audio content cache payload: "+asset.logicalPath;return result;}
            }
            if (!portableRelative(resource.path)) {
                result.error = "Cooker produced a non-portable output path for '"
                    + asset.logicalPath + "': " + resource.path;
                return result;
            }
            StagedFile outputFile;
            outputFile.diskPath = nativeFilesystemPath(objectRoot / "files" / resource.path);
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
            if(resource.type=="DeterministicLevel2D" || asset_format::matchesPath(resource.path,asset_format::Id::DeterministicLevelCooked)) {
                std::string error;entity::DetLevel2D cachedLevel;
                if(resource.type!="DeterministicLevel2D"
                   || !asset_format::isCanonicalPath(resource.path,asset_format::Id::DeterministicLevelCooked)
                   || !validateDetLevelCookedFile(outputFile.diskPath,error,&cachedLevel)) {
                    result.error="Invalid cooked deterministic level cache output: "+resource.path+": "+error;return result;
                }
                if(asset_format::matchesPath(asset.sourcePath,asset_format::Id::DeterministicLevelSource)) {
                    entity::DetLevel2D sourceLevel;
                    if(!entity::loadDetLevel2D(asset.sourcePath,sourceLevel,error)
                       || asset.cacheKey!=computeCacheKey(plan.profile,asset)
                       || entity::encodeDetLevel2D(sourceLevel)!=entity::encodeDetLevel2D(cachedLevel)) {
                        result.error="Cooked deterministic level cache/source identity mismatch: "+asset.logicalPath+": "+error;return result;
                    }
                }
            }
#endif
            outputFile.logicalPath = portable(resource.path);
            outputFile.storage = asset.storage;
            outputFile.chunk = asset.chunk;
            outputFile.type = resource.type;
            outputFile.dependencies = conversion.dependencies;
            outputFile.raw = false;
            outputFile.source = asset.logicalPath;
            outputFile.cacheKey = asset.cacheKey;
            staged.push_back(std::move(outputFile));
        }
    }

    initializeLoaders();
    std::sort(staged.begin(), staged.end(), [](const StagedFile& a,
                                               const StagedFile& b) {
        if (a.logicalPath != b.logicalPath) return a.logicalPath < b.logicalPath;
        return a.source < b.source;
    });
    for (std::size_t index = 1; index < staged.size(); ++index) {
        if (staged[index - 1].logicalPath == staged[index].logicalPath
            && staged[index - 1].diskPath != staged[index].diskPath
            && (staged[index].type != "Texture"
                || staged[index-1].type != staged[index].type
                || staged[index-1].storage != staged[index].storage
                || staged[index-1].chunk != staged[index].chunk
                || hashFiles({staged[index-1].diskPath},"shared-output")
                    != hashFiles({staged[index].diskPath},"shared-output"))) {
            result.error = "Multiple sources publish the same logical asset: "
                + staged[index].logicalPath;
            return result;
        }
    }
    staged.erase(std::unique(staged.begin(), staged.end(),
        [](const StagedFile& a, const StagedFile& b) {
            return a.logicalPath == b.logicalPath;
        }), staged.end());

#if defined(AY_RESOURCE_HAS_PARTICLE)
    // The converter graph and actual payload must both close over staged files.
    std::set<std::string> stagedPaths;
    for(const auto& file:staged) stagedPaths.insert(file.logicalPath);
    for(auto& file:staged) if(file.type=="ParticleEffect" || ayt::asset_format::matchesPath(file.logicalPath, ayt::asset_format::Id::ParticleEffect)) {
        particle::EffectAsset effect; std::string error;
        if(!particle::loadEffectAsset(file.diskPath.string(),effect,&error)) { result.error=error; return result; }
        file.type="ParticleEffect";
        for(const auto& emitter:effect.emitters) if(!emitter.effect.texturePath.empty()) {
            auto path=fs::path(emitter.effect.texturePath).lexically_normal().generic_string();
            if(!stagedPaths.contains(path)) path=(fs::path(file.logicalPath).parent_path()/path).lexically_normal().generic_string();
            if(!stagedPaths.contains(path)) { result.error="Unpublished particle texture: "+emitter.effect.texturePath; return result; }
            auto authored=emitter.effect.texturePath;
            std::replace(authored.begin(),authored.end(),'\\','/');
            if(file.storage==ProjectAssetStorage::Pak && fs::path(authored).lexically_normal().generic_string()!=path) {
                result.error="Cook owner-relative particle texture references before Pak storage: "+authored; return result;
            }
            if(file.storage==ProjectAssetStorage::Pak && !ayt::asset_format::isCanonicalPath(path, ayt::asset_format::Id::Texture)) {
                result.error="Packaged particle textures must be cooked .txr: "+path; return result;
            }
            file.dependencies.push_back({file.logicalPath,path});
        }
    }
#endif

    report(progress, ProjectBuildStage::Stage, 0.78f, "staging content");
    std::set<std::string> audioCredits;
    for(const auto& file:staged)if(file.type=="AudioContent"){AudioContent content;if(!content.load(file.diskPath.string())){result.error="Invalid staged audio content: "+file.logicalPath;return result;}if(!content.credits().empty())audioCredits.insert(content.credits());}
    if(!audioCredits.empty()){std::string notice;for(const auto& credit:audioCredits)notice+=credit+"\n";fs::create_directories(staging/"licenses",fileError);if(fileError||!io::File::atomicWrite((staging/"licenses/audio-content.txt").string(),notice.data(),notice.size())){result.error="Cannot publish audio content attribution";return result;}}
    auto database = ayt::storage::IStorageDatabase::create(
        (contentRoot / "resources.db").string());
    if (!database || !database->isOpen() || !database->beginBatch()) {
        result.error = "Could not create the staged resources database.";
        return result;
    }
    std::map<std::string, std::unique_ptr<ayt::storage::IPackageWriter>> writers;
    Json manifest = {
        {"format", "AYProjectBuildManifest"},
        {"version", 1},
        {"profile", plan.profile.id},
        {"platform", plan.profile.platform},
        {"architecture", plan.profile.architecture},
        {"configuration", plan.profile.configuration},
        {"executable", result.executable.empty()
            ? std::string{} : fs::path(result.executable).filename().string()},
        {"runtimeFiles", runtimeFiles},
        {"files", Json::array()},
    };
    if(!audioCredits.empty())manifest["audioContentCredits"]="licenses/audio-content.txt";
    for (const StagedFile& file : staged) {
        if (!fs::is_regular_file(file.diskPath)) {
            database->rollbackBatch();
            result.error = "Staged source is missing: " + file.diskPath.string();
            return result;
        }
        const std::string contentHash = hashFiles({file.diskPath}, "package-v1");
        const ProjectAssetStorage effectiveStorage = options.skipPackage
            ? ProjectAssetStorage::Loose : file.storage;
        std::string packageName;
        if (effectiveStorage == ProjectAssetStorage::Loose) {
            if (!copyFile(file.diskPath, contentRoot / file.logicalPath,
                          result.error)) {
                database->rollbackBatch();
                return result;
            }
        } else {
            packageName = file.chunk + ".pak";
            auto& writer = writers[file.chunk];
            if (!writer) {
                writer = ayt::storage::IPackageWriter::create(
                    (contentRoot / packageName).string());
                if (!writer) {
                    database->rollbackBatch();
                    result.error = "Could not create package chunk: " + packageName;
                    return result;
                }
                writer->setCompressionAlgo(
                    compressionOf(plan.profile.package.compression));
            }
            if (!writer->addFile(file.diskPath.string(), file.logicalPath)) {
                database->rollbackBatch();
                result.error = "Could not add asset to package: "
                    + file.logicalPath;
                return result;
            }
            ++result.pakFileCount;
        }
        std::string type = file.type;
        if (type.empty()) type = ResourceRegistry::getTypeFromPath(file.logicalPath);
        StagedFile typed = file;
        typed.type = type;
        if (!insertResourceRecord(*database, typed, packageName, contentHash)) {
            database->rollbackBatch();
            result.error = "Could not index staged resource: " + file.logicalPath;
            return result;
        }
        for (const auto& dependency : file.dependencies) {
            if (dependency.from.empty() || dependency.to.empty()) continue;
            database->addDependency(portable(dependency.from),
                                    portable(dependency.to));
        }
        if (effectiveStorage == ProjectAssetStorage::Pak && type.empty()) {
            diagnostic(result.diagnostics,
                ProjectBuildDiagnostic::Severity::Warning,
                file.logicalPath,
                "Raw opaque file is stored in a Pak but has no ResourceRegistry loader.");
        }
        manifest["files"].push_back({
            {"path", file.logicalPath},
            {"source", file.source},
            {"representation", file.raw ? "raw" : "cooked"},
            {"storage", effectiveStorage == ProjectAssetStorage::Pak
                ? "pak:" + file.chunk : "loose"},
            {"type", type},
            {"hash", contentHash},
            {"cookKey", file.cacheKey},
            {"resourceDependencies", file.declaredResources},
            {"scriptDependencies", [&] { std::vector<std::string> paths;
                for (const auto& dep : file.dependencies) if (fs::path(dep.to).extension() == ".logia") paths.push_back(dep.to);
                return paths; }()},
        });
    }
    for (auto& [chunk, writer] : writers) {
        report(progress, ProjectBuildStage::Package, 0.88f,
               "writing " + chunk + ".pak");
        if (!writer->flush()) {
            database->rollbackBatch();
            result.error = "Could not flush package chunk: " + chunk;
            return result;
        }
    }
    // Windows keeps package files locked until the writer objects are
    // destroyed. Release every handle before the staging directory is renamed
    // into the published output.
    writers.clear();
    if (!database->commitBatch()) {
        result.error = "Could not commit the staged resources database.";
        return result;
    }
    database->close();
    database.reset();
    if (!result.executable.empty()) {
        const fs::path stagedRunDirectory = staging
            / plan.profile.run.workingDirectory;
        if (!isUnder(staging, stagedRunDirectory)) {
            result.error = "Run working directory escapes the package output.";
            return result;
        }
        fs::create_directories(stagedRunDirectory, fileError);
        if (fileError) {
            result.error = "Could not stage the run working directory: "
                + fileError.message();
            return result;
        }
    }
    std::vector<std::string> shaderPackages;
    if(!detail::stageProductionShaders(plan,builtArtifact,staging,contentRoot,shaderPackages,result.error)) return result;
    manifest["shaderPackages"]=shaderPackages;
    if(!shaderPackages.empty()) {
        const std::string policy="AYPhoskiaProduction/1\n";
        if(!ayt::io::File::atomicWrite((staging/"Shaders"/"production.policy").string(),policy.data(),policy.size())) {
            result.error="Cannot write production shader policy";return result;
        }
    }
    const std::string encodedManifest = manifest.dump(2) + "\n";
    if (!ayt::io::File::atomicWrite((staging / kPackageManifest).string(),
            encodedManifest.data(), encodedManifest.size())) {
        result.error = "Could not write the project build manifest.";
        return result;
    }

    report(progress, ProjectBuildStage::Publish, 0.95f,
           "publishing package transaction");
    const auto publication = ayt::io::publishDirectory(staging, output,
        [&](const std::string& phase, std::size_t nextAttempt, const std::error_code& error) {
            report(progress, ProjectBuildStage::Publish, 0.95f,
                "Retrying " + phase + " (attempt " + std::to_string(nextAttempt)
                + "; error=" + std::to_string(error.value()) + ")");
        }, "package");
    for (const auto& message : publication.recoveries)
        diagnostic(result.diagnostics, ProjectBuildDiagnostic::Severity::Info, {}, message);
    for (const auto& message : publication.warnings)
        diagnostic(result.diagnostics, ProjectBuildDiagnostic::Severity::Warning, {}, message);
    if (!publication.committed) {
        result.error = publication.error;
        if (!publication.retainedBackup.empty())
            diagnostic(result.diagnostics, ProjectBuildDiagnostic::Severity::Error,
                publication.retainedBackup.generic_string(), result.error + "; old package retained for recovery.");
        return result;
    }

    result.manifestPath = (output / kPackageManifest).string();
    if (!result.executable.empty()) {
        const fs::path stateDirectory = projectRoot / ".ayeditor" / "builds";
        fs::create_directories(stateDirectory, fileError);
        const fs::path runDirectory = output
            / plan.profile.run.workingDirectory;
        const Json state = {
            {"format", "AYProjectBuildState"},
            {"version", 1},
            {"profile", plan.profile.id},
            {"executable", fs::relative(result.executable, projectRoot).generic_string()},
            {"workingDirectory", fs::relative(runDirectory, projectRoot).generic_string()},
            {"arguments", plan.profile.run.arguments},
            {"manifest", fs::relative(result.manifestPath, projectRoot).generic_string()},
        };
        const std::string stateText = state.dump(2) + "\n";
        const fs::path statePath = stateDirectory / "last-success.json";
        if (!ayt::io::File::atomicWrite(statePath.string(),
                stateText.data(), stateText.size())) {
            diagnostic(result.diagnostics,
                ProjectBuildDiagnostic::Severity::Warning, {},
                "Package succeeded but last-success build state could not be written.");
        }
    }
    result.ok = true;
    report(progress, ProjectBuildStage::Done, 1.0f, "project build complete");
    return result;
}

} // namespace ayt::resource
