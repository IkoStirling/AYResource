#include "AYResource/ProjectBuild.h"
#include "AYTest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <vector>

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

} // namespace

TEST_SUITE(ProjectBuildTests)

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
    CHECK(built.ok);
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
    CHECK(first.ok);
    CHECK(first.cookedCount == 1u);
    CHECK(first.cacheHitCount == 0u);
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Content/audio/tone.ayaudio"));

    const ProjectBuildResult second = ProjectBuildExecutor::execute(plan);
    if (!second.ok) {
        std::fprintf(stderr, "[ProjectBuildTests] %s\n", second.error.c_str());
    }
    CHECK(second.ok);
    CHECK(second.cookedCount == 1u);
    CHECK(second.cacheHitCount == 1u);
}

TEST_CASE(existing_code_artifact_is_staged_and_published_for_editor_run)
{
    ProjectBuildCleanup cleanup{"ayproject_build_artifact_test"};
    CHECK(writeText(cleanup.root / "Assets/Data/settings.txt", "test=true\n"));
    CHECK(writeText(cleanup.root / "out/build/Game.exe", "placeholder"));

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
    CHECK(built.ok);
    CHECK(fs::is_regular_file(cleanup.root
        / "out/package/test/Game.exe"));
    CHECK(fs::is_directory(cleanup.root
        / "out/package/test/Content"));

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

TEST_SUITE_END
