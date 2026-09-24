#include "AYResource/ProjectBuild.h"

#include <AYIO/File.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string_view>
#include <utility>

namespace ayt::resource {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

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

} // namespace

ProjectBuildProfile::operator bool() const noexcept
{
    return schemaVersion == kProjectBuildProfileSchemaVersion && !id.empty()
        && !sourcePath.empty();
}

bool ProjectBuildProfile::validate(std::string* error) const
{
    if (error) error->clear();
    const auto reject = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (schemaVersion != kProjectBuildProfileSchemaVersion) {
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

bool ProjectBuildProfile::serialize(
    std::string& jsonText, std::string* error) const
{
    if (error != nullptr) error->clear();
    if (!validate(error)) return false;
    try {
        Json rules = Json::array();
        for (const ProjectBuildRule& rule : content.rules) {
            rules.push_back({
                {"match", rule.match},
                {"transform", transformName(rule.transform)},
                {"storage", rule.storage == ProjectAssetStorage::Pak
                    ? std::string("pak:") + rule.chunk
                    : std::string("loose")},
                {"cookTextures", rule.cookTextures},
            });
        }
        Json root = {
            {"schemaVersion", schemaVersion},
            {"id", id},
            {"target", {
                {"platform", platform},
                {"architecture", architecture},
                {"configuration", configuration},
            }},
            {"code", {
                {"enabled", code.enabled},
                {"backend", code.backend},
                {"cmakeExecutable", code.cmakeExecutable},
                {"sourceDirectory", code.sourceDirectory},
                {"configurePreset", code.configurePreset},
                {"buildPreset", code.buildPreset},
                {"target", code.target},
                {"artifact", code.artifact},
            }},
            {"content", {
                {"assetRoot", content.assetRoot},
                {"outputSubdirectory", content.outputSubdirectory},
                {"defaultTransform", transformName(
                    content.defaultTransform)},
                {"defaultStorage", storageName(content.defaultStorage)},
                {"rules", std::move(rules)},
            }},
            {"cache", {
                {"enabled", cache.enabled},
                {"root", cache.root},
                {"policy", policyName(cache.policy)},
            }},
            {"package", {
                {"output", package.output},
                {"compression", package.compression},
                {"atomic", package.atomic},
            }},
            {"run", {
                {"workingDirectory", run.workingDirectory},
                {"arguments", run.arguments},
            }},
        };
        jsonText = root.dump(2);
        jsonText.push_back('\n');
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Could not serialize build profile: ")
                + exception.what();
        }
        return false;
    }
}

bool ProjectBuildProfile::save(
    const std::string& path, std::string* error) const
{
    if (error != nullptr) error->clear();
    std::string encoded;
    if (!serialize(encoded, error)) return false;
    try {
        const fs::path destination = fs::absolute(path).lexically_normal();
        std::error_code directoryError;
        fs::create_directories(destination.parent_path(), directoryError);
        if (directoryError) {
            if (error != nullptr) {
                *error = "Could not create build profile directory: "
                    + directoryError.message();
            }
            return false;
        }
        if (!ayt::io::File::atomicWrite(
                destination.string(), encoded.data(), encoded.size())) {
            if (error != nullptr) {
                *error = "Atomic build profile save failed: "
                    + destination.string();
            }
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Could not save build profile: ")
                + exception.what();
        }
        return false;
    }
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

} // namespace ayt::resource
