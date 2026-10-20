#pragma once

#include <AYResource/IConverter.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ayt::resource
{

// ============================================================
// ImportJob — offline/editor import orchestration (P5, library core)
//
// FBX/glTF/texture → IConverter → cooked .ay* under an assets cache dir.
// CLI: AYTool/import_tool. Editor: ayt::editor::Importer is a thin wrapper.
// ============================================================

enum class ImportStage {
    Validate = 0,
    CheckCache,
    CacheHit,
    CreateConverter,
    Convert,
    Done,
    Cancelled,
    Failed,
};

struct ImportProgress {
    ImportStage stage = ImportStage::Validate;
    float       fraction = 0.0f;   ///< 0..1 coarse progress
    std::string message;
};

using ImportProgressFn = std::function<void(const ImportProgress&)>;

/// Cooperative cancel token. Checked between stages (not inside Assimp).
struct ImportCancelToken {
    std::atomic<bool> cancelled{false};

    void requestCancel() { cancelled.store(true, std::memory_order_relaxed); }
    bool isCancelled() const { return cancelled.load(std::memory_order_relaxed); }
};

struct ImportOptions {
    std::string sourcePath;                 ///< Source file (.fbx / .gltf / .png / …)
    std::string outputDir;                  ///< Cooked assets root (e.g. ayeditor_cache/assets/)
    /// Optional domain dependency boundary (audio/particle/stats/level); empty
    /// uses that converter's existing Assets discovery policy.
    std::string sourceAssetRoot;
    IConverter::LoadOption loadOption = IConverter::LoadOption::Full;
    bool force = false;                     ///< Skip .dep cache reuse
    /// For .fbx/.gltf/.glb cache hits: require Mesh + Skeleton entries (Editor character path).
    bool requireCharacterAssets = true;
    /// For animation-source cache hits: require at least one Animation entry.
    bool requireAnimationAssets = false;
    /// false (default, dev): textures are referenced raw — the source
    /// png/jpg/… is copied into textures/ with its original extension and
    /// .aymat points at it (TextureLoader decodes with stb at runtime).
    /// true (release cook): full cook to .aytex (BC7 + mips). Mismatch
    /// between the sidecar's textureMode and this flag invalidates the
    /// cached .aydep.json (deterministic cache miss).
    bool cookTextures = false;
    MaterialImportPolicy materialPolicy;
    SourceCoordinatePolicy sourceCoordinates;
    /// Explicit compatibility escape hatch for pre-fingerprint sidecars.
    /// Default imports rebuild them once; runtime assets remain readable.
    bool allowLegacyCache = false;
    /// Stable authoring identity; permits an explicitly bound source recipe to
    /// survive project relocation. It is not a security credential.
    std::string sourceId;
    /// Optional single ASCII [A-Za-z0-9_-] component beneath each typed output
    /// folder. Distinct source keys isolate identically named models/images.
    std::string outputNamespace;
};

struct ImportResult {
    bool ok = false;
    bool usedCache = false;
    bool cancelled = false;
    ConversionResult conversion;
    std::string error;
    std::string depSidecarPath;             ///< Canonical .dep metadata (some domain converters expose their resource sidecar).
};

/// Extension helpers (same table as IConverter::create).
std::string importExtensionOf(const std::string& path);
bool isImportSupportedExtension(const std::string& sourcePath);
/// Canonical fingerprint metadata location for this source/output namespace.
std::string importDependencySidecarPath(const ImportOptions& options);

/// Persisted authoring recipe recovered from a successful import sidecar.
/// Paths are local cache metadata, not portable references stored in a Scene.
struct ImportRecord {
    ImportOptions options;
    ConversionResult conversion;
    std::string sidecarPath;
    std::vector<std::string> inputPaths;
};

/// Read a fingerprinted recipe, preserving `record` on failure. Legacy
/// dependency-only sidecars have no recoverable source/settings and are refused.
bool readImportRecord(const std::string& sidecarPath, ImportRecord& record,
                      std::string* error = nullptr);

/// Verify the stored recipe's source/dependency/output content and settings.
/// Disk hashing belongs on an authoring worker, not a per-frame UI callback.
bool isImportRecordCurrent(const ImportRecord& record);

/// Pure authoring recipe codec. Stores source/output/root references exactly
/// as supplied so a project owner can use portable project-relative paths.
/// Force/cancel/cache-compatibility flags are session actions and not persisted.
/// Parsing performs no filesystem I/O and preserves `options` on failure.
std::string serializeImportRecipe(const ImportOptions& options);
bool parseImportRecipe(const std::string& json, ImportOptions& options,
                       std::string* error = nullptr);

/// Shared authoring data consumed by the Editor and offline project build.
/// Files are .ayeditor/imports/source-*.json, independent of disposable caches.
struct ProjectImportRecipe {
    std::string id;
    std::string path;
    ImportOptions options;
};
/// Bounded UTF-8 reads; resolve relative references against projectRoot.
/// Invalid files are skipped with an error, which build/persistence owners must
/// treat as failure. Absolute external sources are allowed; relative escapes
/// are refused. Conversion and runtime loading are not performed here.
std::vector<ProjectImportRecipe> readProjectImportRecipes(
    const std::string& projectRoot, std::string* error = nullptr);

/// Import into an isolated staging directory, validate, then publish with
/// rollback on failure. Resource paths remain stable while source/subobject
/// names, namespace and output texture mode stay unchanged.
/// Model/image cache metadata fingerprints actual source dependencies, outputs and complete
/// settings. Imports sharing an output root must be serialized by the caller.
/// Publication is not a crash-atomic multi-file snapshot for concurrent readers.
ImportResult importAsset(const ImportOptions& options,
                         ImportProgressFn progress = {},
                         ImportCancelToken* cancel = nullptr);

struct ImportBatchOptions {
    std::vector<std::string> sourcePaths;
    std::string outputDir;
    IConverter::LoadOption loadOption = IConverter::LoadOption::Full;
    bool force = false;
    bool requireCharacterAssets = true;
    bool requireAnimationAssets = false;
    bool stopOnError = false;
    bool cookTextures = false;              ///< see ImportOptions::cookTextures
    MaterialImportPolicy materialPolicy;
    SourceCoordinatePolicy sourceCoordinates;
    bool allowLegacyCache = false;
};

struct ImportBatchResult {
    size_t okCount = 0;
    size_t failCount = 0;
    size_t cacheHitCount = 0;
    size_t cancelledCount = 0;
    std::vector<ImportResult> results;
};

/// Import multiple sources sequentially (shared cancel token).
ImportBatchResult importAssetBatch(const ImportBatchOptions& options,
                                   ImportProgressFn progress = {},
                                   ImportCancelToken* cancel = nullptr);

} // namespace ayt::resource
