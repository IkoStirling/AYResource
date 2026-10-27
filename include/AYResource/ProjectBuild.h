#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ayt::resource {

inline constexpr std::uint32_t kProjectBuildProfileSchemaVersion = 1u;

enum class ProjectAssetTransform : std::uint8_t {
    Auto,
    Raw,
    Cook,
    Exclude,
};

enum class ProjectAssetStorage : std::uint8_t {
    Loose,
    Pak,
};

enum class ProjectCookPolicy : std::uint8_t {
    Auto,
    CacheOnly,
    Force,
    Never,
};

struct ProjectBuildRule {
    std::string match;
    ProjectAssetTransform transform = ProjectAssetTransform::Auto;
    ProjectAssetStorage storage = ProjectAssetStorage::Loose;
    std::string chunk = "core";
    bool cookTextures = true;
};

struct ProjectCodeBuildProfile {
    bool enabled = false;
    std::string backend = "cmake";
    std::string cmakeExecutable = "cmake";
    std::string sourceDirectory = ".";
    std::string configurePreset;
    std::string buildPreset;
    std::string target;
    std::string artifact;
};

struct ProjectContentBuildProfile {
    std::string assetRoot = "Assets";
    std::string outputSubdirectory = "Content";
    /// Optional Assets-relative JSON mapping dialogue documents and referenced
    /// locale/voice/sequence keys to files selected by this build profile.
    std::string dialogueBindings;
    ProjectAssetTransform defaultTransform = ProjectAssetTransform::Raw;
    ProjectAssetStorage defaultStorage = ProjectAssetStorage::Loose;
    std::vector<ProjectBuildRule> rules;
};

struct ProjectCookCacheProfile {
    bool enabled = true;
    std::string root = ".cookCache";
    ProjectCookPolicy policy = ProjectCookPolicy::Auto;
};

struct ProjectPackageProfile {
    std::string output = "out/package";
    std::string compression = "zstd";
    bool atomic = true;
};

struct ProjectRunBuildProfile {
    // Relative to the published package directory, not the project root.
    std::string workingDirectory = ".";
    std::vector<std::string> arguments;
};

// Checked-in, manually editable build profile. Transform (Raw/Cook) and
// storage (Loose/Pak) are deliberately orthogonal.
struct ProjectBuildProfile {
    std::uint32_t schemaVersion = 0;
    std::string id;
    std::string platform;
    std::string architecture;
    std::string configuration;
    ProjectCodeBuildProfile code;
    ProjectContentBuildProfile content;
    ProjectCookCacheProfile cache;
    ProjectPackageProfile package;
    ProjectRunBuildProfile run;
    std::string sourcePath;

    explicit operator bool() const noexcept;
    bool validate(std::string* error = nullptr) const;

    // Canonical checked-in authoring representation shared by editor and CLI.
    bool serialize(std::string& jsonText,
                   std::string* error = nullptr) const;
    bool save(const std::string& path,
              std::string* error = nullptr) const;

    static ProjectBuildProfile load(const std::string& path,
                                    std::string* error = nullptr);
};

struct ProjectBuildDiagnostic {
    enum class Severity : std::uint8_t { Info, Warning, Error };

    Severity severity = Severity::Info;
    std::string asset;
    std::string message;
};

struct ProjectBuildAsset {
    std::string sourcePath;
    std::string logicalPath;
    ProjectAssetTransform transform = ProjectAssetTransform::Raw;
    ProjectAssetStorage storage = ProjectAssetStorage::Loose;
    std::string chunk;
    bool cookTextures = true;
    std::size_t matchedRule = static_cast<std::size_t>(-1);
    /// Cook identity, or the source-byte identity of a selected raw dialogue document.
    std::string cacheKey;
    /// Portable transitive Logia source paths resolved by the planner. Excluded,
    /// missing or non-loose imports are errors; execution rechecks this closure.
    std::vector<std::string> scriptDependencies;
    /// Explicit asset-root-relative resources from the transitive Logia modules.
    /// Transform/storage follow normal project rules; excluded/missing references fail.
    std::vector<std::string> resourceDependencies;
    /// Complete resolved authoring recipe and its project data file. Cooking
    /// preserves names/coordinates/material settings; profile owns destination
    /// and texture cooking. Recipe bytes participate in the cache/plan identity.
    std::string importRecipe;
    std::string importRecipePath;
    /// Physical input closure discovered once by the scene importer while
    /// planning. Source bytes, external buffers and images are hashed; unrelated
    /// neighbouring files do not invalidate the model's cook result.
    std::vector<std::string> importDependencies;
};

struct ProjectBuildPlan {
    ProjectBuildProfile profile;
    std::string projectRoot;
    std::vector<ProjectBuildAsset> assets;
    std::vector<ProjectBuildDiagnostic> diagnostics;

    bool valid() const noexcept;
    std::string serialize(bool pretty = true) const;
};

class ProjectBuildPlanner final {
public:
    /// Particle-enabled authoring validates effect/scene references and hashes
    /// every referenced texture in the owning effect's cook identity.
    /// Script-enabled authoring discovers Logia imports using the shared parser.
    /// Unruled scripts default to raw/loose; explicit incompatible rules fail.
    /// With AYProgression JSON authoring, selected raw dialogue documents are
    /// validated and pinned by source bytes before publication.
    static ProjectBuildPlan create(const ProjectBuildProfile& profile,
                                   const std::string& projectRoot);
};

enum class ProjectBuildStage : std::uint8_t {
    Validate,
    ConfigureCode,
    BuildCode,
    Cook,
    Stage,
    Package,
    Publish,
    Done,
};

struct ProjectBuildProgress {
    ProjectBuildStage stage = ProjectBuildStage::Validate;
    float fraction = 0.0f;
    std::string message;
};

using ProjectBuildProgressFn =
    std::function<void(const ProjectBuildProgress&)>;

struct ProjectBuildExecutionOptions {
    bool dryRun = false;
    bool skipCode = false;
    bool skipPackage = false;
    bool forceCook = false;
    bool cacheOnly = false;
    bool neverCook = false;
};

struct ProjectBuildResult {
    bool ok = false;
    std::size_t rawCount = 0;
    std::size_t cookedCount = 0;
    std::size_t excludedCount = 0;
    std::size_t cacheHitCount = 0;
    std::size_t pakFileCount = 0;
    std::string outputDirectory;
    std::string executable;
    std::string manifestPath;
    std::vector<ProjectBuildDiagnostic> diagnostics;
    std::string error;
};

/// Executes one immutable plan. A code artifact also stages adjacent platform
/// runtime libraries (DLL/.so/.dylib) so the published directory is runnable
/// without reaching back into its build tree. Successful publication writes
/// both a package manifest and `.ayeditor/builds/last-success.json`, allowing
/// Project Run to consume the exact artifact instead of guessing a build
/// directory.
/// Adjacent engine shader packages are verified/staged; selected raw/loose
/// .phoskia programs and modules are compiled into a portable project package.
/// Published shader policy disables runtime compilation even for Debug code.
/// Windows publication/retirement/rollback retry short-lived directory
/// contention with a 250 ms retry scheduling budget and at most 26 attempts
/// per operation; in-flight filesystem calls cannot be interrupted by it.
/// Final failures retain phase, paths and OS error; a failed rollback preserves
/// .previous for recovery by a later build. No non-atomic fallback is used.
class ProjectBuildExecutor final {
public:
    static ProjectBuildResult execute(
        const ProjectBuildPlan& plan,
        const ProjectBuildExecutionOptions& options = {},
        ProjectBuildProgressFn progress = {});
};

} // namespace ayt::resource
