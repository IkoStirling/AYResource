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
    IConverter::LoadOption loadOption = IConverter::LoadOption::Full;
    bool force = false;                     ///< Skip .aydep.json cache reuse
    /// For .fbx/.gltf/.glb cache hits: require Mesh + Skeleton entries (Editor character path).
    bool requireCharacterAssets = true;
    /// false (default, dev): textures are referenced raw — the source
    /// png/jpg/… is copied into textures/ with its original extension and
    /// .aymat points at it (TextureLoader decodes with stb at runtime).
    /// true (release cook): full cook to .aytex (BC7 + mips). Mismatch
    /// between the sidecar's textureMode and this flag invalidates the
    /// cached .aydep.json (deterministic cache miss).
    bool cookTextures = false;
};

struct ImportResult {
    bool ok = false;
    bool usedCache = false;
    bool cancelled = false;
    ConversionResult conversion;
    std::string error;
    std::string depSidecarPath;             ///< Written/reused `<stem>.aydep.json` when applicable
};

/// Extension helpers (same table as IConverter::create).
std::string importExtensionOf(const std::string& path);
bool isImportSupportedExtension(const std::string& sourcePath);

/// Import one source asset into `outputDir` (meshes/, materials/, … + .aydep.json).
ImportResult importAsset(const ImportOptions& options,
                         ImportProgressFn progress = {},
                         ImportCancelToken* cancel = nullptr);

struct ImportBatchOptions {
    std::vector<std::string> sourcePaths;
    std::string outputDir;
    IConverter::LoadOption loadOption = IConverter::LoadOption::Full;
    bool force = false;
    bool requireCharacterAssets = true;
    bool stopOnError = false;
    bool cookTextures = false;              ///< see ImportOptions::cookTextures
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
