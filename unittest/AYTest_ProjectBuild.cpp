#include "AYResource/ProjectBuild.h"
#include "AYTest.h"
#ifdef AY_RESOURCE_TEST_SHADER
#include <AYShader/ShaderBuild.h>
#include <AYShader/detail/PhoskiaSourceSnapshot.h>
#include <AYShader/detail/ShaderRuntimeKey.h>
#include <AYShader/Phoskia.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

using namespace ayt::resource;

namespace {

namespace fs = std::filesystem;

struct ProjectBuildCleanup {
    fs::path root;
    ~ProjectBuildCleanup()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

#if defined(_WIN32)
struct PublicationReadHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    PublicationReadHandle() = default;
    PublicationReadHandle(const PublicationReadHandle&) = delete;
    PublicationReadHandle& operator=(const PublicationReadHandle&) = delete;
    ~PublicationReadHandle() { close(); }
    void close() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
    bool open(const fs::path& path, bool shareDelete = true) {
        close();
        // Attribute-only handles reproduce parent rename contention, but their
        // sharing flags do not enforce a child-file deletion conflict. Use a
        // data-read handle for the distinct backup-cleanup fixture.
        value = CreateFileW(fs::absolute(path).c_str(), shareDelete ? FILE_READ_ATTRIBUTES : GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | (shareDelete ? FILE_SHARE_DELETE : 0), nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        return value != INVALID_HANDLE_VALUE;
    }
};
#endif

bool writeText(const fs::path& path, const std::string& text)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

bool writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

std::vector<std::uint8_t> makeTinyWav()
{
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    constexpr std::uint32_t sampleRate = 22050;
    constexpr std::uint32_t frames = 32;
    constexpr std::uint32_t dataBytes = frames * channels * (bits / 8);

    struct Header {
        char riff[4] = {'R', 'I', 'F', 'F'};
        std::uint32_t fileSize = 36 + dataBytes;
        char wave[4] = {'W', 'A', 'V', 'E'};
        char format[4] = {'f', 'm', 't', ' '};
        std::uint32_t formatSize = 16;
        std::uint16_t audioFormat = 1;
        std::uint16_t channelCount = channels;
        std::uint32_t rate = sampleRate;
        std::uint32_t byteRate = sampleRate * channels * (bits / 8);
        std::uint16_t blockAlign = channels * (bits / 8);
        std::uint16_t bitsPerSample = bits;
        char data[4] = {'d', 'a', 't', 'a'};
        std::uint32_t dataSize = dataBytes;
    } header;

    std::vector<std::uint8_t> result(sizeof(Header) + dataBytes, 0);
    std::memcpy(result.data(), &header, sizeof(Header));
    return result;
}

std::string profileJson(const std::string& output = "out/package/test")
{
    nlohmann::json profile = {
        {"schemaVersion", 1},
        {"id", "windows-test"},
        {"target", {
            {"platform", "windows"},
            {"architecture", "x64"},
            {"configuration", "Development"},
        }},
        {"content", {
            {"assetRoot", "Assets"},
            {"outputSubdirectory", "Content"},
            {"defaultTransform", "raw"},
            {"defaultStorage", "loose"},
            {"rules", nlohmann::json::array({
                {
                    {"match", "Textures/**/*.png"},
                    {"transform", "cook"},
                    {"storage", "pak:textures"},
                    {"cookTextures", true},
                },
                {
                    {"match", "UI/**/*.json"},
                    {"transform", "raw"},
                    {"storage", "pak:ui"},
                },
            })},
        }},
        {"cache", {
            {"root", ".cookCache"},
            {"enabled", true},
            {"policy", "auto"},
        }},
        {"package", {
            {"output", output},
            {"compression", "none"},
            {"atomic", true},
        }},
        {"run", {{"workingDirectory", "."}, {"arguments", nlohmann::json::array()}}},
    };
    return profile.dump(2);
}

#if defined(_WIN32)
ProjectBuildPlan publicationFixture(const fs::path& root, bool oldPackage)
{
    CHECK(writeText(root / "Assets/Data/value.txt", "new"));
    CHECK(writeText(root / "BuildProfiles/test.aybuild.json", profileJson()));
    if (oldPackage) CHECK(writeText(root / "out/package/test/Content/Data/value.txt", "old"));
    std::string error;
    const auto profile = ProjectBuildProfile::load((root / "BuildProfiles/test.aybuild.json").string(), &error);
    CHECK_MSG(static_cast<bool>(profile), error.c_str());
    auto plan = ProjectBuildPlanner::create(profile, root.string());
    CHECK(plan.valid());
    return plan;
}

std::string publicationText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
#endif

std::string skeletonFingerprint(const fs::path& path)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) return {};
    const auto modified = fs::last_write_time(path, error);
    if (error) return std::to_string(size);
    return std::to_string(size) + ":"
        + std::to_string(modified.time_since_epoch().count());
}

nlohmann::json requiredSkeletonRoles()
{
    const char* roles[] = {
        "hips", "spine", "head",
        "leftUpperArm", "leftLowerArm", "leftHand",
        "rightUpperArm", "rightLowerArm", "rightHand",
        "leftUpperLeg", "leftLowerLeg", "leftFoot",
        "rightUpperLeg", "rightLowerLeg", "rightFoot",
    };
    nlohmann::json result = nlohmann::json::object();
    int index = 0;
    for (const char* role : roles) {
        result[role] = {
            {"bonePath", std::string("root/") + role},
            {"sourceIndex", index++},
        };
    }
    return result;
}

nlohmann::json rigProfile(const std::string& fingerprint)
{
    return {
        {"type", "RigProfile"}, {"version", 1},
        {"id", "rig-test-hero"}, {"kind", "mapping"},
        {"source", {
            {"skeleton", "hero.ayskel"},
            {"fingerprint", fingerprint},
        }},
        {"native", false}, {"roles", requiredSkeletonRoles()},
    };
}

std::string rigProfileFingerprint(const nlohmann::json& profile,
                                  const std::string& sourceFingerprint,
                                  const fs::path& profilePath = {})
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
    const auto profileRoles = profile.value("roles", nlohmann::json::object());
    const std::string kind = profile.value("kind", std::string{"mapping"});
    const auto target = profile.value("target", nlohmann::json::object());
    const auto targetRoles = target.value("roles", nlohmann::json::object());
    const auto output = profile.value("output", nlohmann::json::object());
    const auto corrections = profile.value(
        "corrections", nlohmann::json::object());
    fs::path targetPath;
    if (kind == "retarget") {
        targetPath = target.value("skeleton", std::string{});
        if (targetPath.is_relative()) targetPath = profilePath.parent_path() / targetPath;
        targetPath = fs::absolute(targetPath).lexically_normal();
    }
    std::string canonical = "v1|" + sourceFingerprint + "|native="
        + (profile.value("native", false) ? "1" : "0")
        + "|custom="
        + (profile.value("strategy", std::string{}) == "custom" ? "1" : "0")
        + "|kind=" + kind + "|target=" + targetPath.generic_string()
        + "|targetFingerprint=" + skeletonFingerprint(targetPath)
        + "|outputMode=" + (kind == "retarget"
            ? output.value("mode", std::string{"BakeToTarget"})
            : "SemanticNormalize")
        + "|platform=" + (kind == "retarget"
            ? output.value("platform", std::string{"default"}) : "");
    const auto appendQuaternion = [&canonical](const nlohmann::json& value) {
        std::array<float, 4> quaternion{0, 0, 0, 1};
        if (value.is_array() && value.size() == 4u) {
            for (std::size_t index = 0; index < 4u; ++index) {
                if (value[index].is_number()) {
                    quaternion[index] = value[index].get<float>();
                }
            }
        }
        std::ostringstream encoded;
        encoded << std::setprecision(9) << quaternion[0] << ","
            << quaternion[1] << "," << quaternion[2] << ","
            << quaternion[3];
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
        const nlohmann::json empty = nlohmann::json::object();
        const auto& correctionValue = correction != corrections.end()
            && correction->is_object() ? *correction : empty;
        appendQuaternion(correctionValue.value(
            "sourceReferenceOffset", nlohmann::json::array()));
        appendQuaternion(correctionValue.value(
            "targetReferenceOffset", nlohmann::json::array()));
        appendQuaternion(correctionValue.value(
            "axisCorrection", nlohmann::json::array()));
    }
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= prime;
    }
    std::ostringstream text;
    text << "rig-input-" << std::hex << std::setfill('0')
         << std::setw(16) << hash;
    return text.str();
}

std::string scopedFnv(const char* prefix, const std::string& value)
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

} // namespace

TEST_SUITE(ProjectBuildTests)

TEST_CASE(phoskia_default_project_publication_compiles_modules_and_preserves_old_output_on_failure)
{
#ifdef AY_RESOURCE_TEST_SHADER
    ProjectBuildCleanup cleanup{"ayproject_phoskia_production"};
    CHECK(writeText(cleanup.root/"BuildProfiles/test.bld",profileJson()));
    const std::string source="include \"math.phoskia\";\nmaterial Probe { vertex { in p : position; return vec4(p,1.0) } fragment { return vec4(gain(0.25)) } }";
    CHECK(writeText(cleanup.root/"Assets/Shaders/probe.phoskia",source));
    CHECK(writeText(cleanup.root/"Assets/Shaders/math.phoskia","fn gain(v:float):float {return v*2.0;}"));
    std::string error;
    const auto profile=ProjectBuildProfile::load((cleanup.root/"BuildProfiles/test.bld").string(),&error);
    const auto plan=ProjectBuildPlanner::create(profile,cleanup.root.string());
    CHECK(plan.valid());
    const auto built=ProjectBuildExecutor::execute(plan);
    CHECK_MSG(built.ok,built.error.c_str());if(!built.ok)return;
    const auto package=fs::path(built.outputDirectory)/"Shaders/windows-s_5_0/project";
    ayt::shader::ShaderBuildReport report;
    CHECK(ayt::shader::readShaderBuildPackage(package.string(),report));
    CHECK(report.entries.size()==1);if(report.entries.size()!=1)return;
    ayt::shader::phoskia::CompileOptions options;
    options.sourcePath=(cleanup.root/"Assets/Shaders/probe.phoskia").string();
    ayt::shader::phoskia::PhoskiaSourceSnapshot snapshot;std::vector<std::string> errors;
    CHECK(ayt::shader::phoskia::detail::preparePhoskiaSource(source,options,snapshot,errors));
    CHECK(report.entries[0].runtimeKey==ayt::shader::detail::shaderRuntimeKey("phoskia",snapshot.text,{},"windows","s_5_0"));
    const auto manifest=fs::path(built.manifestPath);
    std::ifstream input(manifest);const std::string previous{std::istreambuf_iterator<char>(input),{}};input.close();
    CHECK(previous.find("Shaders/windows-s_5_0/project")!=std::string::npos);
    CHECK(writeText(cleanup.root/"Assets/Shaders/math.phoskia","fn broken("));
    const auto failed=ProjectBuildExecutor::execute(ProjectBuildPlanner::create(profile,cleanup.root.string()));
    CHECK_FALSE(failed.ok);
    std::ifstream preserved(manifest);CHECK(std::string(std::istreambuf_iterator<char>(preserved),{})==previous);
    auto pak=profile;pak.content.defaultStorage=ProjectAssetStorage::Pak;
    CHECK_FALSE(ProjectBuildPlanner::create(pak,cleanup.root.string()).valid());
#else
    SKIP_TEST("AYShader authoring is disabled");
#endif
}

TEST_CASE(publish_retries_after_a_staged_child_file_is_released)
{
#if defined(_WIN32)
    ProjectBuildCleanup cleanup{"publish_transient"};
    const auto plan = publicationFixture(cleanup.root, true);
    PublicationReadHandle held;
    int retries = 0;
    const auto built = ProjectBuildExecutor::execute(plan, {}, [&](const ProjectBuildProgress& p) {
        if (p.message == "publishing package transaction")
            CHECK(held.open(cleanup.root / "out/package/test.staging/Content/resources.db"));
        if (p.message.find("Retrying publish package") == 0) { ++retries; held.close(); }
    });
    CHECK_MSG(built.ok, built.error.c_str());
    CHECK(retries > 0);
    CHECK(publicationText(cleanup.root / "out/package/test/Content/Data/value.txt") == "new");
    CHECK_FALSE(fs::exists(cleanup.root / "out/package/test.previous"));
    CHECK_FALSE(fs::exists(cleanup.root / "out/package/test.staging"));
#else
    SKIP_TEST("Windows directory contention fixture");
#endif
}

TEST_CASE(retirement_retries_after_an_old_child_file_is_released)
{
#if defined(_WIN32)
    ProjectBuildCleanup cleanup{"retire_transient"};
    const auto plan = publicationFixture(cleanup.root, true);
    PublicationReadHandle held;
    int retries = 0;
    const auto built = ProjectBuildExecutor::execute(plan, {}, [&](const ProjectBuildProgress& p) {
        if (p.message == "publishing package transaction")
            CHECK(held.open(cleanup.root / "out/package/test/Content/Data/value.txt"));
        if (p.message.find("Retrying retire previous package") == 0) { ++retries; held.close(); }
    });
    CHECK_MSG(built.ok, built.error.c_str());
    CHECK(retries > 0);
    CHECK(publicationText(cleanup.root / "out/package/test/Content/Data/value.txt") == "new");
#else
    SKIP_TEST("Windows directory contention fixture");
#endif
}

TEST_CASE(persistent_publish_contention_times_out_and_restores_the_old_package)
{
#if defined(_WIN32)
    ProjectBuildCleanup cleanup{"publish_persistent"};
    const auto plan = publicationFixture(cleanup.root, true);
    PublicationReadHandle held;
    int retries = 0;
    const auto built = ProjectBuildExecutor::execute(plan, {}, [&](const ProjectBuildProgress& p) {
        if (p.message == "publishing package transaction")
            CHECK(held.open(cleanup.root / "out/package/test.staging/Content/resources.db"));
        if (p.message.find("Retrying publish package") == 0) ++retries;
    });
    CHECK_FALSE(built.ok); CHECK(retries > 0);
    CHECK(built.error.find("Could not publish package") != std::string::npos);
    CHECK(built.error.find("retry budget exhausted") != std::string::npos);
    CHECK(built.error.find("source=") != std::string::npos);
    CHECK(built.error.find("target=") != std::string::npos);
    CHECK(built.error.find("attempts=") != std::string::npos);
    CHECK(built.error.find("rollback failed") == std::string::npos);
    CHECK(publicationText(cleanup.root / "out/package/test/Content/Data/value.txt") == "old");
    CHECK_FALSE(fs::exists(cleanup.root / "out/package/test.previous"));
#else
    SKIP_TEST("Windows directory contention fixture");
#endif
}

TEST_CASE(persistent_retirement_contention_preserves_the_published_package)
{
#if defined(_WIN32)
    ProjectBuildCleanup cleanup{"retire_persistent"};
    const auto plan = publicationFixture(cleanup.root, true);
    PublicationReadHandle held;
    const auto built = ProjectBuildExecutor::execute(plan, {}, [&](const ProjectBuildProgress& p) {
        if (p.message == "publishing package transaction")
            CHECK(held.open(cleanup.root / "out/package/test/Content/Data/value.txt"));
    });
    CHECK_FALSE(built.ok);
    CHECK(built.error.find("Could not retire previous package") != std::string::npos);
    CHECK(built.error.find("retry budget exhausted") != std::string::npos);
    CHECK(publicationText(cleanup.root / "out/package/test/Content/Data/value.txt") == "old");
    CHECK_FALSE(fs::exists(cleanup.root / "out/package/test.previous"));
#else
    SKIP_TEST("Windows directory contention fixture");
#endif
}

TEST_CASE(failed_rollback_preserves_the_backup_and_a_later_build_recovers_it)
{
#if defined(_WIN32)
    ProjectBuildCleanup cleanup{"rollback_persistent"};
    const auto plan = publicationFixture(cleanup.root, true);
    PublicationReadHandle staged, previous;
    const auto failed = ProjectBuildExecutor::execute(plan, {}, [&](const ProjectBuildProgress& p) {
        if (p.message == "publishing package transaction")
            CHECK(staged.open(cleanup.root / "out/package/test.staging/Content/resources.db"));
        if (p.message.find("Retrying publish package") == 0 && previous.value == INVALID_HANDLE_VALUE)
            CHECK(previous.open(cleanup.root / "out/package/test.previous/Content/Data/value.txt"));
    });
    CHECK_FALSE(failed.ok);
    CHECK(failed.error.find("rollback failed") != std::string::npos);
    CHECK(failed.error.find("Could not restore previous package") != std::string::npos);
    CHECK(publicationText(cleanup.root / "out/package/test.previous/Content/Data/value.txt") == "old");
    CHECK_FALSE(fs::exists(cleanup.root / "out/package/test"));
    staged.close();
    const auto blockedRecovery = ProjectBuildExecutor::execute(plan);
    CHECK_FALSE(blockedRecovery.ok);
    CHECK(blockedRecovery.error.find("Could not recover previous package") != std::string::npos);
    CHECK(publicationText(cleanup.root / "out/package/test.previous/Content/Data/value.txt") == "old");
    CHECK_FALSE(fs::exists(cleanup.root / "out/package/test"));
    previous.close();
    // Recovery must restore the old package BEFORE a second failed publication.
    const auto second = ProjectBuildExecutor::execute(plan, {}, [&](const ProjectBuildProgress& p) {
        if (p.message == "publishing package transaction")
            CHECK(staged.open(cleanup.root / "out/package/test.staging/Content/resources.db"));
    });
    CHECK_FALSE(second.ok);
    CHECK(second.error.find("rollback failed") == std::string::npos);
    CHECK(publicationText(cleanup.root / "out/package/test/Content/Data/value.txt") == "old");
    staged.close();
    const auto final = ProjectBuildExecutor::execute(plan);
    CHECK_MSG(final.ok, final.error.c_str());
    CHECK(publicationText(cleanup.root / "out/package/test/Content/Data/value.txt") == "new");
    CHECK_FALSE(fs::exists(cleanup.root / "out/package/test.previous"));
#else
    SKIP_TEST("Windows directory contention fixture");
#endif
}

TEST_CASE(rollback_retries_after_the_old_backup_child_is_released)
{
#if defined(_WIN32)
    ProjectBuildCleanup cleanup{"rollback_transient"};
    const auto plan = publicationFixture(cleanup.root, true);
    PublicationReadHandle staged, previous;
    int rollbackRetries = 0;
    const auto built = ProjectBuildExecutor::execute(plan, {}, [&](const ProjectBuildProgress& p) {
        if (p.message == "publishing package transaction")
            CHECK(staged.open(cleanup.root / "out/package/test.staging/Content/resources.db"));
        if (p.message.find("Retrying publish package") == 0 && previous.value == INVALID_HANDLE_VALUE)
            CHECK(previous.open(cleanup.root / "out/package/test.previous/Content/Data/value.txt"));
        if (p.message.find("Retrying restore previous package") == 0) {
            ++rollbackRetries; previous.close();
        }
    });
    CHECK_FALSE(built.ok); CHECK(rollbackRetries > 0);
    CHECK(built.error.find("rollback failed") == std::string::npos);
    CHECK(publicationText(cleanup.root / "out/package/test/Content/Data/value.txt") == "old");
    CHECK_FALSE(fs::exists(cleanup.root / "out/package/test.previous"));
#else
    SKIP_TEST("Windows directory contention fixture");
#endif
}

TEST_CASE(committed_package_remains_successful_with_a_backup_cleanup_warning)
{
#if defined(_WIN32)
    ProjectBuildCleanup cleanup{"backup_cleanup_locked"};
    const auto plan = publicationFixture(cleanup.root, true);
    PublicationReadHandle staged, previous;
    const auto built = ProjectBuildExecutor::execute(plan, {}, [&](const ProjectBuildProgress& p) {
        if (p.message == "publishing package transaction")
            CHECK(staged.open(cleanup.root / "out/package/test.staging/Content/resources.db"));
        if (p.message.find("Retrying publish package") == 0 && previous.value == INVALID_HANDLE_VALUE) {
            CHECK(previous.open(cleanup.root / "out/package/test.previous/Content/Data/value.txt", false));
            staged.close();
        }
    });
    CHECK_MSG(built.ok, built.error.c_str());
    CHECK(built.error.empty());
    CHECK(publicationText(cleanup.root / "out/package/test/Content/Data/value.txt") == "new");
    CHECK(publicationText(cleanup.root / "out/package/test.previous/Content/Data/value.txt") == "old");
    CHECK(std::any_of(built.diagnostics.begin(), built.diagnostics.end(), [](const auto& d) {
        return d.severity == ProjectBuildDiagnostic::Severity::Warning
            && d.message.find("Could not clean previous package") != std::string::npos
            && d.message.find("retry budget exhausted") != std::string::npos;
    }));
#else
    SKIP_TEST("Windows backup cleanup fixture");
#endif
}

TEST_CASE(profile_round_trip_uses_canonical_serialization)
{
    ProjectBuildCleanup cleanup{"ayproject_build_profile_roundtrip"};
    const fs::path path = cleanup.root
        / "BuildProfiles/roundtrip.aybuild.json";
    CHECK(writeText(path, profileJson()));

    std::string error;
    ProjectBuildProfile profile = ProjectBuildProfile::load(
        path.string(), &error);
    CHECK(static_cast<bool>(profile));
    profile.id = "roundtrip-edited";
    profile.content.rules.push_back(ProjectBuildRule{
        "UI/**/*.json", ProjectAssetTransform::Raw,
        ProjectAssetStorage::Pak, "interface", false});
    CHECK(profile.save(path.string(), &error));

    const ProjectBuildProfile reloaded = ProjectBuildProfile::load(
        path.string(), &error);
    CHECK(static_cast<bool>(reloaded));
    CHECK(reloaded.id == "roundtrip-edited");
    CHECK(reloaded.content.rules.size() == profile.content.rules.size());
    const ProjectBuildRule& rule = reloaded.content.rules.back();
    CHECK(rule.match == "UI/**/*.json");
    CHECK(rule.transform == ProjectAssetTransform::Raw);
    CHECK(rule.storage == ProjectAssetStorage::Pak);
    CHECK(rule.chunk == "interface");
    CHECK_FALSE(rule.cookTextures);
}

TEST_CASE(profile_rules_are_manual_ordered_and_content_addressed)
{
    ProjectBuildCleanup cleanup{"ayproject_build_profile_test"};
    CHECK(writeText(cleanup.root / "Assets/UI/main.ui.json", "{}"));
    CHECK(writeText(cleanup.root / "Assets/Textures/Hero/diffuse.png", "pixels-a"));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json", profileJson()));

    std::string error;
    const ProjectBuildProfile profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    CHECK(static_cast<bool>(profile));
    CHECK(error.empty());
    CHECK(profile.content.rules.size() == 2u);

    ProjectBuildPlan plan = ProjectBuildPlanner::create(
        profile, cleanup.root.string());
    CHECK(plan.valid());
    CHECK(plan.assets.size() == 2u);
    const auto texture = std::find_if(plan.assets.begin(), plan.assets.end(),
        [](const ProjectBuildAsset& value) {
            return value.logicalPath.find("diffuse.png") != std::string::npos;
        });
    CHECK(texture != plan.assets.end());
    CHECK(texture->transform == ProjectAssetTransform::Cook);
    CHECK(texture->storage == ProjectAssetStorage::Pak);
    CHECK(texture->chunk == "textures");
    CHECK(!texture->cacheKey.empty());
    const std::string firstKey = texture->cacheKey;

    CHECK(writeText(cleanup.root / "Assets/Textures/Hero/diffuse.png", "pixels-b"));
    plan = ProjectBuildPlanner::create(profile, cleanup.root.string());
    const auto changed = std::find_if(plan.assets.begin(), plan.assets.end(),
        [](const ProjectBuildAsset& value) {
            return value.logicalPath.find("diffuse.png") != std::string::npos;
        });
    CHECK(changed != plan.assets.end());
    CHECK(changed->cacheKey != firstKey);
}

TEST_CASE(raw_files_can_be_mixed_between_loose_and_pak_storage)
{
    ProjectBuildCleanup cleanup{"ayproject_build_raw_test"};
    CHECK(writeText(cleanup.root / "Assets/Data/settings.txt", "setting=true\n"));
    CHECK(writeText(cleanup.root / "Assets/UI/main.ui.json", "{}"));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json", profileJson()));

    std::string error;
    const ProjectBuildProfile profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        profile, cleanup.root.string());
    CHECK(plan.valid());

    const ProjectBuildResult built = ProjectBuildExecutor::execute(plan);
    if (!built.ok) {
        std::fprintf(stderr, "[ProjectBuildTests] %s\n", built.error.c_str());
    }
    CHECK_MSG(built.ok, built.error.c_str());
    CHECK(built.rawCount == 2u);
    CHECK(built.cookedCount == 0u);
    CHECK(built.pakFileCount == 1u);
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/Data/settings.txt"));
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/ui.pak"));
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/resources.db"));
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/build-manifest.json"));

    if (!built.ok) return;

    std::ifstream input(cleanup.root
        / "out/package/test/build-manifest.json", std::ios::binary);
    nlohmann::json manifest;
    input >> manifest;
    CHECK(manifest["format"] == "AYProjectBuildManifest");
    CHECK(manifest["files"].size() == 2u);
}

TEST_CASE(actor_classes_and_scripts_form_a_checked_loose_release_closure)
{
    ProjectBuildCleanup cleanup{"ayproject_build_actor_closure_test"};
    CHECK(writeText(cleanup.root / "Assets/actors/Base.ayactor",
        R"({"type":"ay.actorClass","schemaVersion":3,"id":"Base","script":"actors/Base.logia","properties":{},"components":[{"$type":"Camera","$instanceId":"11111111111111111111111111111111"},{"$type":"Camera","$instanceId":"22222222222222222222222222222222"}]})"));
    CHECK(writeText(cleanup.root / "Assets/actors/Child.act",
        R"({"type":"ay.actorClass","schemaVersion":2,"id":"Child","parent":"actors/Base.ayactor","script":"","properties":{},"components":[]})"));
    CHECK(writeText(cleanup.root / "Assets/actors/Base.logia",
        "script Base { on_start() {} }\n"));
    CHECK(writeText(cleanup.root / "Assets/worlds/Main.ayscene",
        R"({"entities":[{"components":[{"$type":"ActorInstanceComponent","classPath":"actors/Child.act"}]}]})"));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json", profileJson()));
    std::string error;
    auto profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    auto plan = ProjectBuildPlanner::create(profile, cleanup.root.string());
    CHECK(plan.valid());
    const auto built = ProjectBuildExecutor::execute(plan);
    CHECK_MSG(built.ok, built.error.c_str());
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/actors/Child.act"));
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/actors/Base.logia"));

    CHECK(writeText(cleanup.root / "Assets/actors/Base.ayactor",
        R"({"type":"ay.actorClass","schemaVersion":2,"id":"Base","parent":"actors/Child.act","script":"actors/Base.logia","properties":{},"components":[]})"));
    plan = ProjectBuildPlanner::create(profile, cleanup.root.string());
    CHECK_FALSE(plan.valid());
    CHECK(writeText(cleanup.root / "Assets/actors/Base.ayactor",
        R"({"type":"ay.actorClass","schemaVersion":2,"id":"Base","script":"actors/Base.logia","properties":{},"components":[]})"));
    plan = ProjectBuildPlanner::create(profile, cleanup.root.string());
    CHECK(plan.valid());
    std::error_code ignored;
    fs::remove(cleanup.root / "Assets/actors/Base.logia", ignored);
    const auto stale = ProjectBuildExecutor::execute(plan);
    CHECK_FALSE(stale.ok);
    CHECK(stale.error.find("Actor content") != std::string::npos);
}

TEST_CASE(cache_only_policy_rejects_missing_cooked_object)
{
    ProjectBuildCleanup cleanup{"ayproject_build_cache_only_test"};
    CHECK(writeText(cleanup.root / "Assets/Textures/Hero/diffuse.png", "not-a-png"));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json", profileJson()));
    std::string error;
    ProjectBuildProfile profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    profile.cache.policy = ProjectCookPolicy::CacheOnly;
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        profile, cleanup.root.string());
    const ProjectBuildResult built = ProjectBuildExecutor::execute(plan);
    CHECK(!built.ok);
    CHECK(built.error.find("cache miss") != std::string::npos);
}

TEST_CASE(repeated_build_reuses_content_addressed_cook_result)
{
    ProjectBuildCleanup cleanup{"ayproject_build_cache_hit_test"};
    CHECK(writeBytes(cleanup.root / "Assets/Audio/tone.wav", makeTinyWav()));

    nlohmann::json profile = nlohmann::json::parse(profileJson());
    profile["content"]["rules"] = nlohmann::json::array({
        {
            {"match", "Audio/**/*.wav"},
            {"transform", "cook"},
            {"storage", "loose"},
        },
    });
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profile.dump(2)));

    std::string error;
    const ProjectBuildProfile loaded = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        loaded, cleanup.root.string());
    CHECK(plan.valid());

    const ProjectBuildResult first = ProjectBuildExecutor::execute(plan);
    if (!first.ok) {
        std::fprintf(stderr, "[ProjectBuildTests] %s\n", first.error.c_str());
    }
    CHECK_MSG(first.ok, first.error.c_str());
    CHECK(first.cookedCount == 1u);
    CHECK(first.cacheHitCount == 0u);
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/audio/tone.aud"));

    const ProjectBuildResult second = ProjectBuildExecutor::execute(plan);
    if (!second.ok) {
        std::fprintf(stderr, "[ProjectBuildTests] %s\n", second.error.c_str());
    }
    CHECK_MSG(second.ok, second.error.c_str());
    CHECK(second.cookedCount == 1u);
    CHECK(second.cacheHitCount == 1u);
}

TEST_CASE(existing_code_artifact_is_staged_and_published_for_editor_run)
{
    ProjectBuildCleanup cleanup{"ayproject_build_artifact_test"};
    CHECK(writeText(cleanup.root / "Assets/Data/settings.txt", "test=true\n"));
    CHECK(writeText(cleanup.root / "out/build/Game.exe", "placeholder"));
    CHECK(writeText(cleanup.root / "out/build/runtime.dll", "runtime"));
    CHECK(writeText(cleanup.root / "out/build/debug.pdb", "symbols"));

    nlohmann::json profile = nlohmann::json::parse(profileJson());
    profile["code"] = {
        {"enabled", true},
        {"backend", "cmake"},
        {"configurePreset", "unused-configure"},
        {"buildPreset", "unused-build"},
        {"target", "Game"},
        {"artifact", "out/build/Game.exe"},
    };
    profile["run"]["workingDirectory"] = "Content";
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profile.dump(2)));

    std::string error;
    const ProjectBuildProfile loaded = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        loaded, cleanup.root.string());
    ProjectBuildExecutionOptions options;
    options.skipCode = true;
    const ProjectBuildResult built = ProjectBuildExecutor::execute(
        plan, options);
    if (!built.ok) {
        std::fprintf(stderr, "[ProjectBuildTests] %s\n", built.error.c_str());
    }
    CHECK_MSG(built.ok, built.error.c_str());
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Game.exe"));
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/runtime.dll"));
    CHECK_FALSE(fs::exists(cleanup.root
        / "out/package/test/debug.pdb"));
    CHECK(fs::is_directory(cleanup.root
        / "out/package/test/Content"));

    std::ifstream manifestInput(cleanup.root
        / "out/package/test/build-manifest.json", std::ios::binary);
    nlohmann::json manifest;
    manifestInput >> manifest;
    CHECK(manifest["runtimeFiles"] == nlohmann::json::array({"runtime.dll"}));

    std::ifstream stateInput(cleanup.root
        / ".ayeditor/builds/last-success.json", std::ios::binary);
    nlohmann::json state;
    stateInput >> state;
    CHECK(state["format"] == "AYProjectBuildState");
    CHECK(state["executable"] == "out/package/test/Game.exe");
    CHECK(state["workingDirectory"] == "out/package/test/Content");
}

TEST_CASE(non_atomic_publication_is_rejected_by_schema_v1)
{
    ProjectBuildCleanup cleanup{"ayproject_build_atomic_test"};
    CHECK(writeText(cleanup.root / "Assets/Data/settings.txt", "test=true\n"));
    nlohmann::json profile = nlohmann::json::parse(profileJson());
    profile["package"]["atomic"] = false;
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profile.dump(2)));

    std::string error;
    const ProjectBuildProfile loaded = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    CHECK(!static_cast<bool>(loaded));
    CHECK(error.find("atomic") != std::string::npos);
}

TEST_CASE(package_and_cache_directories_cannot_overlap)
{
    ProjectBuildCleanup cleanup{"ayproject_build_overlap_test"};
    CHECK(writeText(cleanup.root / "Assets/Data/settings.txt", "test=true\n"));
    nlohmann::json profile = nlohmann::json::parse(profileJson());
    profile["cache"]["root"] = "out/package";
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profile.dump(2)));

    std::string error;
    const ProjectBuildProfile loaded = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    CHECK(!static_cast<bool>(loaded));
    CHECK(error.find("overlap") != std::string::npos);
}

TEST_CASE(skeleton_release_gate_requires_legacy_mapping_migration)
{
    ProjectBuildCleanup cleanup{"ayproject_build_skeleton_unbaked_test"};
    const fs::path skeleton = cleanup.root / "Assets/Characters/hero.ayskel";
    CHECK(writeText(skeleton, "source-skeleton"));
    const nlohmann::json mapping = {
        {"type", "SkeletonMapping"}, {"version", 1},
        {"skeleton", "hero.ayskel"},
        {"sourceFingerprint", skeletonFingerprint(skeleton)},
        {"native", false}, {"roles", nlohmann::json::object()},
    };
    CHECK(writeText(cleanup.root / "Assets/Characters/hero.aysmap",
                    mapping.dump(2)));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profileJson()));

    std::string error;
    const ProjectBuildProfile profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        profile, cleanup.root.string());
    CHECK_FALSE(plan.valid());
    CHECK(std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(),
        [](const ProjectBuildDiagnostic& item) {
            return item.message.find("skeletonRigProfileMigrationRequired")
                != std::string::npos;
        }));
    const ProjectBuildResult built = ProjectBuildExecutor::execute(plan);
    CHECK_FALSE(built.ok);
}

TEST_CASE(skeleton_release_gate_packages_only_verified_baked_outputs)
{
    ProjectBuildCleanup cleanup{"ayproject_build_skeleton_ready_test"};
    const fs::path character = cleanup.root / "Assets/Characters";
    const fs::path skeleton = character / "hero.ayskel";
    const fs::path animation = character / "walk.ayanm";
    const fs::path mesh = character / "hero.aymesh";
    const fs::path mask = character / "upper.aymask";
    const fs::path bakedSkeleton = character / "Baked/hero.baked.skl";
    const fs::path bakedAnimation = character / "Baked/walk.baked.anm";
    const fs::path bakedMesh = character / "Baked/hero.baked.msh";
    const fs::path bakedMask = character / "Baked/upper.baked.skm";
    const fs::path staleAnimation = character / "Baked/old.baked.anm";
    CHECK(writeText(skeleton, "source-skeleton"));
    CHECK(writeText(animation, "source-animation"));
    CHECK(writeText(mesh, "source-mesh"));
    CHECK(writeText(mask, "source-mask"));
    CHECK(writeText(bakedSkeleton, "cleaned-skeleton"));
    CHECK(writeText(bakedAnimation, "rewritten-animation"));
    CHECK(writeText(bakedMesh, "rewritten-mesh"));
    CHECK(writeText(bakedMask, "rewritten-mask"));
    CHECK(writeText(staleAnimation, "stale-animation"));
    const std::string fingerprint = skeletonFingerprint(skeleton);
    const nlohmann::json mapping = rigProfile(fingerprint);
    CHECK(writeText(character / "hero.rig", mapping.dump(2)));
    CHECK(writeText(character / "hero.aysmap", "legacy-mapping"));
    nlohmann::json receipt = {
        {"type", "SkeletonBakeResult"}, {"version", 2}, {"generation", 1},
        {"sourceFingerprint", fingerprint},
        {"profileFingerprint", rigProfileFingerprint(mapping, fingerprint)},
        {"outputMode", "SemanticNormalize"}, {"platform", ""}, {"scope", ""},
        {"outputs", nlohmann::json::array({
            fs::absolute(bakedSkeleton).lexically_normal().generic_string(),
            fs::absolute(bakedAnimation).lexically_normal().generic_string(),
            fs::absolute(bakedMesh).lexically_normal().generic_string(),
            fs::absolute(bakedMask).lexically_normal().generic_string(),
        })},
        {"artifacts", nlohmann::json::array({
            {{"kind", "skeleton"},
             {"source", fs::absolute(skeleton).lexically_normal().generic_string()},
             {"sourceFingerprint", skeletonFingerprint(skeleton)},
             {"output", fs::absolute(bakedSkeleton).lexically_normal().generic_string()}},
            {{"kind", "animation"},
             {"source", fs::absolute(animation).lexically_normal().generic_string()},
             {"sourceFingerprint", skeletonFingerprint(animation)},
             {"output", fs::absolute(bakedAnimation).lexically_normal().generic_string()}},
            {{"kind", "mesh"},
             {"source", fs::absolute(mesh).lexically_normal().generic_string()},
             {"sourceFingerprint", skeletonFingerprint(mesh)},
             {"output", fs::absolute(bakedMesh).lexically_normal().generic_string()}},
            {{"kind", "skeletonMask"},
             {"source", fs::absolute(mask).lexically_normal().generic_string()},
             {"sourceFingerprint", skeletonFingerprint(mask)},
             {"output", fs::absolute(bakedMask).lexically_normal().generic_string()}},
        })},
        {"dryRun", {
            {"dependencies", nlohmann::json::array({{
                {"kind", "animation"}, {"impact", "affected"},
                {"path", fs::absolute(animation).lexically_normal().generic_string()},
            }, {
                {"kind", "mesh"}, {"impact", "affected"},
                {"path", fs::absolute(mesh).lexically_normal().generic_string()},
            }, {
                {"kind", "skeletonMask"}, {"impact", "affected"},
                {"path", fs::absolute(mask).lexically_normal().generic_string()},
            }})},
        }},
    };
    CHECK(writeText(character / "Baked/hero.bake-result.json",
                    receipt.dump(2)));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profileJson()));

    std::string error;
    const ProjectBuildProfile profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        profile, cleanup.root.string());
    CHECK(plan.valid());
    const auto transformOf = [&plan](const std::string& suffix) {
        const auto found = std::find_if(plan.assets.begin(), plan.assets.end(),
            [&suffix](const ProjectBuildAsset& asset) {
                return asset.logicalPath.ends_with(suffix);
            });
        return found == plan.assets.end()
            ? ProjectAssetTransform::Auto : found->transform;
    };
    CHECK(transformOf("Characters/hero.ayskel")
        == ProjectAssetTransform::Exclude);
    CHECK(transformOf("Characters/hero.rig")
        == ProjectAssetTransform::Exclude);
    CHECK(transformOf("Characters/hero.aysmap")
        == ProjectAssetTransform::Exclude);
    CHECK(transformOf("Characters/walk.ayanm")
        == ProjectAssetTransform::Exclude);
    CHECK(transformOf("Characters/hero.aymesh")
        == ProjectAssetTransform::Exclude);
    CHECK(transformOf("Characters/upper.aymask")
        == ProjectAssetTransform::Exclude);
    CHECK(transformOf("Characters/Baked/hero.bake-result.json")
        == ProjectAssetTransform::Exclude);
    CHECK(transformOf("Characters/Baked/hero.baked.skl")
        == ProjectAssetTransform::Raw);
    CHECK(transformOf("Characters/Baked/walk.baked.anm")
        == ProjectAssetTransform::Raw);
    CHECK(transformOf("Characters/Baked/hero.baked.msh")
        == ProjectAssetTransform::Raw);
    CHECK(transformOf("Characters/Baked/upper.baked.skm")
        == ProjectAssetTransform::Raw);
    CHECK(transformOf("Characters/Baked/old.baked.anm")
        == ProjectAssetTransform::Exclude);

    const ProjectBuildResult built = ProjectBuildExecutor::execute(plan);
    if (!built.ok) {
        std::fprintf(stderr, "[SkeletonReleaseGate] %s\n", built.error.c_str());
    }
    CHECK_MSG(built.ok, built.error.c_str());
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/Characters/Baked/hero.baked.skl"));
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/Characters/Baked/walk.baked.anm"));
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/Characters/Baked/hero.baked.msh"));
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/Characters/Baked/upper.baked.skm"));
    CHECK_FALSE(fs::exists(cleanup.root
        / "out/package/test/Content/Characters/hero.ayskel"));
    CHECK_FALSE(fs::exists(cleanup.root
        / "out/package/test/Content/Characters/walk.ayanm"));
    CHECK_FALSE(fs::exists(cleanup.root
        / "out/package/test/Content/Characters/Baked/old.baked.anm"));
}

TEST_CASE(skeleton_release_gate_rechecks_source_at_execution_time)
{
    ProjectBuildCleanup cleanup{"ayproject_build_skeleton_stale_test"};
    const fs::path character = cleanup.root / "Assets/Characters";
    const fs::path skeleton = character / "hero.ayskel";
    const fs::path bakedSkeleton = character / "Baked/hero.baked.skl";
    CHECK(writeText(skeleton, "source-skeleton"));
    CHECK(writeText(bakedSkeleton, "cleaned-skeleton"));
    const std::string fingerprint = skeletonFingerprint(skeleton);
    const nlohmann::json mapping = rigProfile(fingerprint);
    CHECK(writeText(character / "hero.rig", mapping.dump(2)));
    nlohmann::json receipt = {
        {"type", "SkeletonBakeResult"}, {"version", 2},
        {"sourceFingerprint", fingerprint},
        {"profileFingerprint", rigProfileFingerprint(mapping, fingerprint)},
        {"outputMode", "SemanticNormalize"}, {"platform", ""}, {"scope", ""},
        {"outputs", nlohmann::json::array({
            fs::absolute(bakedSkeleton).lexically_normal().generic_string(),
        })},
        {"artifacts", nlohmann::json::array({{
            {"kind", "skeleton"},
            {"source", fs::absolute(skeleton).lexically_normal().generic_string()},
            {"sourceFingerprint", skeletonFingerprint(skeleton)},
            {"output", fs::absolute(bakedSkeleton).lexically_normal().generic_string()},
        }})},
        {"dryRun", {{"dependencies", nlohmann::json::array()}}},
    };
    CHECK(writeText(character / "Baked/hero.bake-result.json",
                    receipt.dump(2)));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profileJson()));
    std::string error;
    const ProjectBuildProfile profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        profile, cleanup.root.string());
    CHECK(plan.valid());
    CHECK(writeText(skeleton, "source-skeleton-changed"));
    const ProjectBuildResult built = ProjectBuildExecutor::execute(plan);
    CHECK_FALSE(built.ok);
    CHECK(built.error.find("Skeleton release gate") != std::string::npos);
}

TEST_CASE(skeleton_release_gate_accepts_scoped_retarget_receipt)
{
    ProjectBuildCleanup cleanup{"ayproject_build_skeleton_retarget_test"};
    const fs::path character = cleanup.root / "Assets/Characters";
    const fs::path skeleton = character / "hero.ayskel";
    const fs::path target = cleanup.root / "Fixtures/target.ayskel";
    CHECK(writeText(skeleton, "source-skeleton"));
    CHECK(writeText(target, "target-skeleton"));
    const std::string fingerprint = skeletonFingerprint(skeleton);
    nlohmann::json profile = rigProfile(fingerprint);
    profile["kind"] = "retarget";
    profile["target"] = {
        {"skeleton", fs::relative(target, character).generic_string()},
        {"fingerprint", skeletonFingerprint(target)},
        {"roles", requiredSkeletonRoles()},
    };
    profile["output"] = {{"mode", "BakeToTarget"}, {"platform", "test"}};
    const fs::path profilePath = character / "hero.rig";
    CHECK(writeText(profilePath, profile.dump(2)));
    const std::string scope = scopedFnv("rt-",
        fs::absolute(target).lexically_normal().generic_string()
            + "|BakeToTarget|test");
    const fs::path bakedSkeleton = character / "Baked"
        / ("hero." + scope + ".baked.skl");
    const fs::path receiptPath = character / "Baked"
        / ("hero." + scope + ".bake-result.json");
    CHECK(writeText(bakedSkeleton, "retargeted-skeleton"));
    const nlohmann::json receipt = {
        {"type", "SkeletonBakeResult"}, {"version", 2},
        {"sourceFingerprint", fingerprint},
        {"profileFingerprint", rigProfileFingerprint(
            profile, fingerprint, profilePath)},
        {"targetSkeleton", fs::absolute(target).lexically_normal().generic_string()},
        {"outputMode", "BakeToTarget"}, {"platform", "test"},
        {"scope", scope},
        {"outputs", nlohmann::json::array({
            fs::absolute(bakedSkeleton).lexically_normal().generic_string(),
        })},
        {"artifacts", nlohmann::json::array({{
            {"kind", "skeleton"},
            {"source", fs::absolute(skeleton).lexically_normal().generic_string()},
            {"sourceFingerprint", skeletonFingerprint(skeleton)},
            {"output", fs::absolute(bakedSkeleton).lexically_normal().generic_string()},
        }})},
        {"dryRun", {{"dependencies", nlohmann::json::array()}}},
    };
    CHECK(writeText(receiptPath, receipt.dump(2)));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profileJson()));
    std::string error;
    const ProjectBuildProfile buildProfile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        buildProfile, cleanup.root.string());
    CHECK(plan.valid());
    const ProjectBuildResult built = ProjectBuildExecutor::execute(plan);
    CHECK_MSG(built.ok, built.error.c_str());
    CHECK(fs::is_regular_file(cleanup.root / "out/package/test/Content"
        / fs::relative(bakedSkeleton, cleanup.root / "Assets")));
}

TEST_CASE(skeleton_release_gate_rechecks_rig_profile_at_execution_time)
{
    ProjectBuildCleanup cleanup{"ayproject_build_rig_profile_stale_test"};
    const fs::path character = cleanup.root / "Assets/Characters";
    const fs::path skeleton = character / "hero.ayskel";
    const fs::path bakedSkeleton = character / "Baked/hero.baked.skl";
    CHECK(writeText(skeleton, "source-skeleton"));
    CHECK(writeText(bakedSkeleton, "cleaned-skeleton"));
    const std::string fingerprint = skeletonFingerprint(skeleton);
    nlohmann::json mapping = rigProfile(fingerprint);
    CHECK(writeText(character / "hero.rig", mapping.dump(2)));
    const nlohmann::json receipt = {
        {"type", "SkeletonBakeResult"}, {"version", 2},
        {"sourceFingerprint", fingerprint},
        {"profileFingerprint", rigProfileFingerprint(mapping, fingerprint)},
        {"outputMode", "SemanticNormalize"}, {"platform", ""}, {"scope", ""},
        {"outputs", nlohmann::json::array({
            fs::absolute(bakedSkeleton).lexically_normal().generic_string(),
        })},
        {"artifacts", nlohmann::json::array({{
            {"kind", "skeleton"},
            {"source", fs::absolute(skeleton).lexically_normal().generic_string()},
            {"sourceFingerprint", skeletonFingerprint(skeleton)},
            {"output", fs::absolute(bakedSkeleton).lexically_normal().generic_string()},
        }})},
        {"dryRun", {{"dependencies", nlohmann::json::array()}}},
    };
    CHECK(writeText(character / "Baked/hero.bake-result.json",
                    receipt.dump(2)));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profileJson()));
    std::string error;
    const ProjectBuildProfile profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        profile, cleanup.root.string());
    CHECK(plan.valid());

    mapping["roles"]["head"]["bonePath"] = "root/reassignedHead";
    CHECK(writeText(character / "hero.rig", mapping.dump(2)));
    const ProjectBuildResult built = ProjectBuildExecutor::execute(plan);
    CHECK_FALSE(built.ok);
    CHECK(built.error.find("Skeleton release gate") != std::string::npos);
}

TEST_CASE(skeleton_release_gate_rechecks_dependency_closure_at_execution_time)
{
    ProjectBuildCleanup cleanup{"ayproject_build_skeleton_dependency_stale_test"};
    const fs::path character = cleanup.root / "Assets/Characters";
    const fs::path skeleton = character / "hero.ayskel";
    const fs::path animation = character / "walk.ayanm";
    const fs::path bakedSkeleton = character / "Baked/hero.baked.skl";
    const fs::path bakedAnimation = character / "Baked/walk.baked.anm";
    CHECK(writeText(skeleton, "source-skeleton"));
    CHECK(writeText(animation, "source-animation"));
    CHECK(writeText(bakedSkeleton, "cleaned-skeleton"));
    CHECK(writeText(bakedAnimation, "rewritten-animation"));
    const std::string fingerprint = skeletonFingerprint(skeleton);
    const nlohmann::json mapping = rigProfile(fingerprint);
    CHECK(writeText(character / "hero.rig", mapping.dump(2)));
    const nlohmann::json receipt = {
        {"type", "SkeletonBakeResult"}, {"version", 2},
        {"sourceFingerprint", fingerprint},
        {"profileFingerprint", rigProfileFingerprint(mapping, fingerprint)},
        {"outputMode", "SemanticNormalize"}, {"platform", ""}, {"scope", ""},
        {"outputs", nlohmann::json::array({
            fs::absolute(bakedSkeleton).lexically_normal().generic_string(),
            fs::absolute(bakedAnimation).lexically_normal().generic_string(),
        })},
        {"artifacts", nlohmann::json::array({
            {{"kind", "skeleton"},
             {"source", fs::absolute(skeleton).lexically_normal().generic_string()},
             {"sourceFingerprint", skeletonFingerprint(skeleton)},
             {"output", fs::absolute(bakedSkeleton).lexically_normal().generic_string()}},
            {{"kind", "animation"},
             {"source", fs::absolute(animation).lexically_normal().generic_string()},
             {"sourceFingerprint", skeletonFingerprint(animation)},
             {"output", fs::absolute(bakedAnimation).lexically_normal().generic_string()}},
        })},
        {"dryRun", {{"dependencies", nlohmann::json::array({{
            {"kind", "animation"}, {"impact", "affected"},
            {"path", fs::absolute(animation).lexically_normal().generic_string()},
        }})}}},
    };
    CHECK(writeText(character / "Baked/hero.bake-result.json",
                    receipt.dump(2)));
    CHECK(writeText(cleanup.root / "BuildProfiles/test.aybuild.json",
                    profileJson()));
    std::string error;
    const ProjectBuildProfile profile = ProjectBuildProfile::load(
        (cleanup.root / "BuildProfiles/test.aybuild.json").string(), &error);
    const ProjectBuildPlan plan = ProjectBuildPlanner::create(
        profile, cleanup.root.string());
    CHECK(plan.valid());
    CHECK(writeText(animation, "source-animation-changed"));
    const ProjectBuildResult built = ProjectBuildExecutor::execute(plan);
    CHECK_FALSE(built.ok);
    CHECK(built.error.find("Skeleton release gate") != std::string::npos);
}

TEST_SUITE_END
