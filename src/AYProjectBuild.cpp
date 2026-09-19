#include "AYResource/ProjectBuild.h"

#include "AYResource/ImportJob.h"
#include "AYResource/ResourceBootstrap.h"
#include "AYResource/ResourceRegistry.h"

#include <AYCrypto.h>
#include <AYIO/File.h>
#include <AYStorage/IPackageWriter.h>
#include <AYStorage/IStorageDatabase.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

constexpr std::uint32_t kProfileVersion = 1;
constexpr const char* kPackageManifest = "build-manifest.json";

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
    std::error_code rootError;
    std::error_code candidateError;
    fs::path normalizedRoot = fs::weakly_canonical(root, rootError);
    fs::path normalizedCandidate = fs::weakly_canonical(
        candidate, candidateError);
    if (rootError) normalizedRoot = fs::absolute(root).lexically_normal();
    if (candidateError) {
        normalizedCandidate = fs::absolute(candidate).lexically_normal();
    }
    const fs::path relative = normalizedCandidate.lexically_relative(
        normalizedRoot);
    return !relative.empty() && !relative.is_absolute()
        && *relative.begin() != "..";
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
    return filename.ends_with(".baked.ayskel");
}

bool isSkeletonAuthoringMetadata(const fs::path& path)
{
    const std::string filename = lower(path.filename().string());
    const std::string extension = lower(path.extension().string());
    return extension == ".ayrig" || extension == ".aysmap"
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

std::string rigProfileFingerprint(const Json& profile,
                                  const std::string& sourceFingerprint)
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
    std::string canonical = "v1|" + sourceFingerprint + "|native="
        + (profile.value("native", false) ? "1" : "0");
    for (const std::string_view role : roles) {
        canonical += "|";
        canonical += role;
        canonical += "=";
        const auto value = profileRoles.find(std::string(role));
        canonical += value != profileRoles.end() && value->is_object()
            ? value->value("bonePath", std::string{"-"}) : "-";
    }
    return fnvHex("rig-input-", canonical);
}

struct SkeletonReleaseGateResult {
    bool valid = false;
    std::unordered_set<std::string> excludedPaths;
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
    mappingPath.replace_extension(".ayrig");
    if (!fs::is_regular_file(mappingPath)) {
        fs::path legacyPath = skeletonPath;
        legacyPath.replace_extension(".aysmap");
        if (fs::is_regular_file(legacyPath)) {
            reject("skeletonRigProfileMigrationRequired",
                "Legacy .aysmap must be opened and saved as .ayrig before packaging.");
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
        || mapping.value("kind", std::string{}) != "mapping"
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
    const std::string fingerprint = skeletonSourceFingerprint(skeletonPath);
    if (fingerprint.empty()
        || source.value("fingerprint", std::string{}) != fingerprint) {
        reject("skeletonRigProfileStale",
            "Source skeleton changed after its Rig Profile was saved.");
        return result;
    }
    const std::string profileFingerprint = rigProfileFingerprint(
        mapping, fingerprint);

    const fs::path receiptPath = skeletonPath.parent_path() / "Baked"
        / (skeletonPath.stem().string() + ".bake-result.json");
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
        || receipt.value("version", 0u) != 1u
        || receipt.value("sourceFingerprint", std::string{}) != fingerprint
        || receipt.value("profileFingerprint", std::string{})
            != profileFingerprint) {
        reject("skeletonBakeReceiptInvalid",
            "Bake result manifest does not match the current skeleton and Rig Profile.");
        return result;
    }
    bool hasSkeletonOutput = false;
    std::size_t animationOutputCount = 0u;
    const Json outputs = receipt.value("outputs", Json::array());
    if (!outputs.is_array() || outputs.empty()) {
        reject("skeletonBakeOutputMissing",
            "Bake result manifest contains no outputs.");
        return result;
    }
    for (const Json& value : outputs) {
        if (!value.is_string()) {
            reject("skeletonBakeOutputInvalid",
                "Bake result manifest contains an invalid output path.");
            return result;
        }
        const fs::path output = value.get<std::string>();
        if (!fs::is_regular_file(output) || !isUnder(assetsRoot, output)) {
            reject("skeletonBakeOutputMissing",
                "Baked output is missing or outside the package asset root: "
                    + portable(output));
            return result;
        }
        if (isBakedSkeletonPath(output)) hasSkeletonOutput = true;
        if (lower(output.extension().string()) == ".ayanm") {
            ++animationOutputCount;
        }
    }
    if (!hasSkeletonOutput) {
        reject("skeletonBakeOutputMissing",
            "Bake result has no cleaned .baked.ayskel output.");
        return result;
    }

    std::size_t animationDependencyCount = 0u;
    const Json dependencies = receipt.value("dryRun", Json::object())
        .value("dependencies", Json::array());
    if (dependencies.is_array()) {
        for (const Json& dependency : dependencies) {
            if (!dependency.is_object()
                || dependency.value("kind", std::string{}) != "animation") {
                continue;
            }
            ++animationDependencyCount;
            const fs::path source = dependency.value("path", std::string{});
            if (!source.empty()) {
                result.excludedPaths.insert(
                    portable(fs::absolute(source).lexically_normal()));
            }
        }
    }
    if (animationOutputCount < animationDependencyCount) {
        reject("skeletonAnimationBakeIncomplete",
            "Not every affected animation has a baked output.");
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

std::vector<fs::path> sourceClosure(const fs::path& source)
{
    std::vector<fs::path> paths{source};
    const std::string extension = lower(source.extension().string());
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
    return value.dump();
}

std::string computeCacheKey(const ProjectBuildProfile& profile,
                            const ProjectBuildAsset& asset)
{
    return hashFiles(sourceClosure(asset.sourcePath),
                     cacheDiscriminator(profile, asset));
}

ayt::storage::CompressionAlgo compressionOf(const std::string& value)
{
    const std::string normalized = lower(value);
    if (normalized == "none") return ayt::storage::CompressionAlgo::None;
    if (normalized == "lz4") return ayt::storage::CompressionAlgo::Lz4;
    return ayt::storage::CompressionAlgo::Zstd;
}

std::wstring wide(const std::string& value)
{
#if defined(_WIN32)
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

std::wstring quote(std::wstring value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(character);
        } else {
            output.append(slashes, L'\\');
            output.push_back(character);
        }
        slashes = 0;
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

int runProcess(const std::vector<std::string>& arguments,
               const fs::path& workingDirectory)
{
    if (arguments.empty()) return -1;
#if defined(_WIN32)
    std::wstring command;
    for (const std::string& argument : arguments) {
        if (!command.empty()) command.push_back(L' ');
        command += quote(wide(argument));
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working = workingDirectory.wstring();
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
            TRUE, 0, nullptr, working.c_str(), &startup, &process)) {
        return -static_cast<int>(GetLastError());
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
#else
    std::string command;
    for (const std::string& argument : arguments) {
        if (!command.empty()) command.push_back(' ');
        command.push_back('\'');
        for (char ch : argument) {
            if (ch == '\'') command += "'\\''";
            else command.push_back(ch);
        }
        command.push_back('\'');
    }
    const fs::path previous = fs::current_path();
    fs::current_path(workingDirectory);
    const int code = std::system(command.c_str());
    fs::current_path(previous);
    return code;
#endif
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
    fs::create_directories(destination.parent_path(), fileError);
    if (fileError) {
        error = "Could not create staging directory: " + fileError.message();
        return false;
    }
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing,
                  fileError);
    if (fileError) {
        error = "Could not stage '" + source.string() + "': "
            + fileError.message();
        return false;
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
        if (!fs::is_regular_file(objectRoot / "files" / resource.path)) {
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
    fileError.clear();
    fs::rename(temporary, objectRoot, fileError);
    if (fileError) {
        error = "Could not publish cook cache object: " + fileError.message();
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

ProjectBuildProfile::operator bool() const noexcept
{
    return schemaVersion == kProfileVersion && !id.empty()
        && !sourcePath.empty();
}

bool ProjectBuildProfile::validate(std::string* error) const
{
    if (error) error->clear();
    const auto reject = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (schemaVersion != kProfileVersion) {
        return reject("Unsupported build profile schemaVersion: "
            + std::to_string(schemaVersion));
    }
    if (id.empty()) return reject("Build profile id cannot be empty.");
    if (platform.empty() || architecture.empty() || configuration.empty()) {
        return reject("Build target requires platform, architecture, and configuration.");
    }
    if (!portableRelative(content.assetRoot)
        || !portableRelative(content.outputSubdirectory)
        || !portableRelative(cache.root)
        || !portableRelative(package.output)
        || !portableRelative(run.workingDirectory)) {
        return reject("Build profile paths must be non-empty project-relative paths.");
    }
    if (package.output == "." || cache.root == ".") {
        return reject("Package output and cook cache cannot be the project root.");
    }
    if (nestedPath(package.output, content.assetRoot)
        || nestedPath(cache.root, content.assetRoot)) {
        return reject("Package output and cook cache cannot live below the asset root.");
    }
    if (package.output == cache.root
        || nestedPath(package.output, cache.root)
        || nestedPath(cache.root, package.output)) {
        return reject("Package output and cook cache cannot overlap.");
    }
    if (package.output == ".ayeditor"
        || nestedPath(package.output, ".ayeditor")
        || cache.root == ".ayeditor"
        || nestedPath(cache.root, ".ayeditor")) {
        return reject("Package output and cook cache cannot overwrite editor state.");
    }
    if (code.enabled) {
        if (lower(code.backend) != "cmake") {
            return reject("Only the cmake code build backend is currently supported.");
        }
        if (!portableRelative(code.sourceDirectory)) {
            return reject("Code sourceDirectory must stay inside the project root.");
        }
        if (code.configurePreset.empty() || code.buildPreset.empty()
            || code.target.empty() || !portableRelative(code.artifact)) {
            return reject("CMake code build requires configurePreset, buildPreset, target, and relative artifact.");
        }
    }
    for (std::size_t index = 0; index < content.rules.size(); ++index) {
        const ProjectBuildRule& rule = content.rules[index];
        if (rule.match.empty()) {
            return reject("Build rule " + std::to_string(index)
                + " has an empty match pattern.");
        }
        try { (void)globRegex(rule.match); }
        catch (const std::regex_error&) {
            return reject("Build rule " + std::to_string(index)
                + " has an invalid glob pattern.");
        }
        if (rule.storage == ProjectAssetStorage::Pak
            && !safeChunkName(rule.chunk)) {
            return reject("Pak build rule " + std::to_string(index)
                + " requires a portable filename-safe chunk name.");
        }
    }
    const std::string compression = lower(package.compression);
    if (compression != "none" && compression != "lz4"
        && compression != "zstd") {
        return reject("Package compression must be none, lz4, or zstd.");
    }
    if (!package.atomic) {
        return reject("schemaVersion 1 requires atomic package publication.");
    }
    return true;
}

ProjectBuildProfile ProjectBuildProfile::load(
    const std::string& path, std::string* error)
{
    if (error) error->clear();
    ProjectBuildProfile result;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            if (error) *error = "Build profile was not found: " + path;
            return {};
        }
        Json root;
        input >> root;
        result.schemaVersion = root.value("schemaVersion", 0u);
        result.id = root.value("id", std::string{});
        const Json target = root.value("target", Json::object());
        result.platform = target.value("platform", std::string{});
        result.architecture = target.value("architecture", std::string{});
        result.configuration = target.value("configuration", std::string{});

        const Json code = root.value("code", Json::object());
        result.code.enabled = code.value("enabled", false);
        result.code.backend = code.value("backend", std::string("cmake"));
        result.code.cmakeExecutable = code.value(
            "cmakeExecutable", std::string("cmake"));
        result.code.sourceDirectory = code.value(
            "sourceDirectory", std::string("."));
        result.code.configurePreset = code.value(
            "configurePreset", std::string{});
        result.code.buildPreset = code.value("buildPreset", std::string{});
        result.code.target = code.value("target", std::string{});
        result.code.artifact = code.value("artifact", std::string{});

        const Json content = root.value("content", Json::object());
        result.content.assetRoot = content.value(
            "assetRoot", std::string("Assets"));
        result.content.outputSubdirectory = content.value(
            "outputSubdirectory", std::string("Content"));
        std::string text = content.value("defaultTransform", std::string("raw"));
        if (!parseTransform(text, result.content.defaultTransform)) {
            if (error) *error = "Unknown content.defaultTransform: " + text;
            return {};
        }
        text = content.value("defaultStorage", std::string("loose"));
        if (!parseStorage(text, result.content.defaultStorage)) {
            if (error) *error = "Unknown content.defaultStorage: " + text;
            return {};
        }
        const Json rules = content.value("rules", Json::array());
        if (!rules.is_array()) {
            if (error) *error = "content.rules must be an array.";
            return {};
        }
        for (const Json& encoded : rules) {
            if (!encoded.is_object()) {
                if (error) *error = "Each content rule must be an object.";
                return {};
            }
            ProjectBuildRule rule;
            rule.match = encoded.value("match", std::string{});
            text = encoded.value("transform", std::string("auto"));
            if (!parseTransform(text, rule.transform)) {
                if (error) *error = "Unknown rule transform: " + text;
                return {};
            }
            text = encoded.value("storage", std::string("loose"));
            if (!parseStorage(text, rule.storage, &rule.chunk)) {
                if (error) *error = "Unknown rule storage: " + text;
                return {};
            }
            rule.chunk = encoded.value("chunk", rule.chunk);
            rule.cookTextures = encoded.value("cookTextures", true);
            result.content.rules.push_back(std::move(rule));
        }

        const Json cache = root.value("cache", Json::object());
        result.cache.enabled = cache.value("enabled", true);
        result.cache.root = cache.value("root", std::string(".cookCache"));
        text = cache.value("policy", std::string("auto"));
        if (!parsePolicy(text, result.cache.policy)) {
            if (error) *error = "Unknown cache.policy: " + text;
            return {};
        }

        const Json package = root.value("package", Json::object());
        result.package.output = package.value(
            "output", std::string("out/package"));
        result.package.compression = package.value(
            "compression", std::string("zstd"));
        result.package.atomic = package.value("atomic", true);

        const Json run = root.value("run", Json::object());
        result.run.workingDirectory = run.value(
            "workingDirectory", std::string("."));
        if (const auto arguments = run.find("arguments");
            arguments != run.end()) {
            if (!arguments->is_array()) {
                if (error) *error = "run.arguments must be an array.";
                return {};
            }
            for (const Json& argument : *arguments) {
                if (!argument.is_string()) {
                    if (error) *error = "run.arguments values must be strings.";
                    return {};
                }
                result.run.arguments.push_back(argument.get<std::string>());
            }
        }
        result.sourcePath = fs::absolute(path).lexically_normal().string();
        std::string validation;
        if (!result.validate(&validation)) {
            if (error) *error = std::move(validation);
            return {};
        }
        return result;
    } catch (const std::exception& exception) {
        if (error) *error = std::string("Invalid build profile: ")
            + exception.what();
        return {};
    }
}

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
    for (const fs::path& file : files) {
        if (lower(file.extension().string()) != ".ayskel"
            || isBakedSkeletonPath(file)) {
            continue;
        }
        SkeletonReleaseGateResult gate = validateSkeletonReleaseGate(
            file, assetsRoot, plan.diagnostics);
        skeletonAuthoringExclusions.insert(
            gate.excludedPaths.begin(), gate.excludedPaths.end());
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
        if (asset.transform == ProjectAssetTransform::Auto) {
            asset.transform = isImportSupportedExtension(asset.sourcePath)
                ? ProjectAssetTransform::Cook
                : ProjectAssetTransform::Raw;
        }
        const std::string absoluteFile = portable(
            fs::absolute(file).lexically_normal());
        if (isSkeletonAuthoringMetadata(file)
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
        plan.assets.push_back(std::move(asset));
    }
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
    for (const ProjectBuildAsset& asset : plan.assets) {
        const fs::path source(asset.sourcePath);
        if (lower(source.extension().string()) != ".ayskel"
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
    if (fs::exists(staging)) fs::remove_all(staging, fileError);
    fileError.clear();
    fs::create_directories(contentRoot, fileError);
    if (fileError) {
        result.error = "Could not create staging directory: " + fileError.message();
        return result;
    }

    fs::path builtArtifact;
    if (plan.profile.code.enabled) {
        const fs::path sourceDirectory =
            projectRoot / plan.profile.code.sourceDirectory;
        if (!options.skipCode) {
            report(progress, ProjectBuildStage::ConfigureCode, 0.02f,
                   "configuring CMake project");
            const int configure = runProcess({
                plan.profile.code.cmakeExecutable,
                "--preset", plan.profile.code.configurePreset,
                "-S", sourceDirectory.string(),
            }, projectRoot);
            if (configure != 0) {
                result.error = "CMake configure failed with exit code "
                    + std::to_string(configure) + ".";
                return result;
            }
            report(progress, ProjectBuildStage::BuildCode, 0.08f,
                   "building CMake target " + plan.profile.code.target);
            const int build = runProcess({
                plan.profile.code.cmakeExecutable,
                "--build", "--preset", plan.profile.code.buildPreset,
                "--target", plan.profile.code.target,
            }, projectRoot);
            if (build != 0) {
                result.error = "CMake build failed with exit code "
                    + std::to_string(build) + ".";
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
        const fs::path objectRoot = cacheRoot / "objects"
            / asset.cacheKey.substr(0, 2) / asset.cacheKey;
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
            const fs::path temporary = cacheRoot / "staging"
                / (asset.cacheKey + ".tmp");
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
            import.outputDir = (temporary / "files").string();
            import.force = true;
            import.requireCharacterAssets = false;
            import.cookTextures = asset.cookTextures;
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
        for (const auto& resource : conversion.resources) {
            if (!portableRelative(resource.path)) {
                result.error = "Cooker produced a non-portable output path for '"
                    + asset.logicalPath + "': " + resource.path;
                return result;
            }
            StagedFile outputFile;
            outputFile.diskPath = objectRoot / "files" / resource.path;
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
            && staged[index - 1].diskPath != staged[index].diskPath) {
            result.error = "Multiple sources publish the same logical asset: "
                + staged[index].logicalPath;
            return result;
        }
    }
    staged.erase(std::unique(staged.begin(), staged.end(),
        [](const StagedFile& a, const StagedFile& b) {
            return a.logicalPath == b.logicalPath;
        }), staged.end());

    report(progress, ProjectBuildStage::Stage, 0.78f, "staging content");
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
        {"files", Json::array()},
    };
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
    const std::string encodedManifest = manifest.dump(2) + "\n";
    if (!ayt::io::File::atomicWrite((staging / kPackageManifest).string(),
            encodedManifest.data(), encodedManifest.size())) {
        result.error = "Could not write the project build manifest.";
        return result;
    }

    report(progress, ProjectBuildStage::Publish, 0.95f,
           "publishing package transaction");
    const fs::path previous = fs::path(output.string() + ".previous");
    if (fs::exists(previous)) fs::remove_all(previous, fileError);
    fileError.clear();
    if (fs::exists(output)) {
        fs::rename(output, previous, fileError);
        if (fileError) {
            result.error = "Could not retire previous package: "
                + fileError.message();
            return result;
        }
    }
    fs::rename(staging, output, fileError);
    if (fileError) {
        if (fs::exists(previous)) {
            std::error_code restoreError;
            fs::rename(previous, output, restoreError);
        }
        result.error = "Could not publish package: " + fileError.message();
        return result;
    }
    if (fs::exists(previous)) fs::remove_all(previous, fileError);

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
