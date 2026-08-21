#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <AYMath/MathTypes.h>

namespace ayt::resource
{

inline constexpr const char* kFbxImporterContractTag =
    "fbx-assimp-explicit-source-space-lh-yup-zfwd-ccw-uvtop-m-v8";

// Signed cardinal directions used by scene importers.  Auto keeps the source
// format importer's declared axis metadata; explicit directions let an editor
// expose the same Up/Forward controls as DCC-oriented engine importers.
enum class ImportAxis : uint8_t {
    PositiveX = 0,
    NegativeX,
    PositiveY,
    NegativeY,
    PositiveZ,
    NegativeZ,
};

enum class ImportHandedness : uint8_t {
    Left = 0,
    Right,
};

enum class SourceCoordinateMode : uint8_t {
    Auto = 0,
    Manual,
};

struct SourceCoordinatePolicy {
    SourceCoordinateMode mode = SourceCoordinateMode::Auto;
    ImportAxis up = ImportAxis::PositiveY;
    ImportAxis forward = ImportAxis::PositiveZ;
    ImportHandedness handedness = ImportHandedness::Left;
    // 0 = use the source file's unit metadata.  Otherwise this is the number
    // of meters represented by one source unit (for example 0.01 for cm).
    float metersPerUnit = 0.0f;
    // Stable caller-provided discriminator.  It is persisted in .aydep and
    // compared on cache lookup so changing an import preset forces a recook.
    std::string tag;
};

std::string sourceCoordinatePolicyCacheTag(const SourceCoordinatePolicy& policy);

// Optional per-source material policy supplied by editor/project config.
// FBX often exposes a TransparencyFactor texture for every material while
// omitting a usable alpha-mode flag, so importers need an explicit override
// instead of guessing from texture pixels.
struct MaterialImportPolicy {
    std::string tag;
    std::string opaqueIndices;
    std::string maskIndices;
    std::string blendIndices;
    std::string doubleSidedIndices;
    // Stable overrides. Entries are separated with ';' so material names may
    // contain spaces or commas. Name rules take precedence over legacy index
    // rules and survive FBX material reordering.
    std::string opaqueNames;
    std::string maskNames;
    std::string blendNames;
    std::string doubleSidedNames;
};

// ===== ConversionResult — 转换结果结构 =====
// 用于 Converter 输出文件 + 依赖信息
struct ConversionResult {
    // 先定义嵌套类型
    struct ConvertedResource {
        ayt::math::FGuid guid;  // 资源唯一标识
        std::string path;
        std::string type;
        // F2.3: was int64_t mismatched with IResource::sizeInBytes() (size_t).
        // Coerce to size_t's underlying unsigned counterpart here so the
        // shared payload (toJson/fromJson, cache stats, cook log) has
        // a single numeric type. JSON round-trip uses unsigned strtoull.
        uint64_t size = 0;
    };

    struct Dependency {
        std::string from;
        std::string to;
    };

    // 然后再使用它们
    std::vector<ConvertedResource> resources;
    std::vector<Dependency> dependencies;

    // Texture mode of the produced assets; empty = legacy sidecar (written
    // before textureMode existed — accepted under both import modes so old
    // cooked caches stay usable in dev). "raw" = textures referenced/copied
    // with original extension (ImportOptions.cookTextures=false); "cook" =
    // full BC7+mips .aytex. JSON round-trips as an optional field.
    std::string textureMode;
    // Cache discriminator for material classification. A requested non-empty
    // tag invalidates legacy sidecars that lack the imported surface modes.
    std::string materialPolicyTag;
    // Importer-owned cache discriminator for coordinate, skeleton palette and
    // binary semantic changes. Old FBX sidecars without it must be rebuilt.
    std::string importerContractTag;
    // Effective source-coordinate preset. Empty means a legacy sidecar.
    std::string sourceCoordinateTag;

    // JSON 序列化（离线模式用）
    std::string toJson() const;
    static ConversionResult fromJson(const std::string& json);
};

// ===== IConverter — 资源转换接口 =====
class IConverter {
	public:
	    virtual ~IConverter() = default;

	    // ===== 加载选项 =====
	    enum class LoadOption {
	        MeshOnly = 0,  // 最小处理
	        Full = 1       // 完整处理
	    };

	    // ===== 配置 =====
	    virtual void setSourcePath(const std::string& path) = 0;
	    virtual void setOutputDir(const std::string& dir) = 0;
	    virtual void setLoadOption(LoadOption option) = 0;

	    // Dev raw-reference mode (see ImportOptions::cookTextures).
	    // Default no-op: converters that care (FBXConverter → its parser
	    // + TextureConverter rawCopy) override this.
	    virtual void setCookTextures(bool /*cook*/) {}
	    virtual void setMaterialImportPolicy(const MaterialImportPolicy& /*policy*/) {}
	    virtual void setSourceCoordinatePolicy(const SourceCoordinatePolicy& /*policy*/) {}

	    // ===== 转换 =====
	    virtual ConversionResult convert() = 0;
	    virtual bool isValid() const = 0;

	    // ===== 信息 =====
	    virtual const char* getSourceType() const = 0;

	    // ===== 工厂方法 =====
	    static std::unique_ptr<IConverter> create(const std::string& sourcePath);

	    // ===== 模板方法区 =====

	protected:
	    // 检查是否跳过（文件已存在则返回 true）
	    // FBXConverter 等协调者返回 false
	    virtual bool shouldSkip() { return false; }

	    // 执行转换（FBXConverter 等协调者返回空，由子类覆写）
	    virtual ConversionResult convertInternal() { return {}; }

	    // 写入结果（FBXConverter 等协调者不需要实现）
	    virtual void writeResult(const ConversionResult&) {}

	    // 获取输出文件路径（FBXConverter 等协调者返回空）
	    virtual std::string getOutputPath() const { return {}; }
};

// F2.2: optional batch entry point split out from IConverter. Implementors
// accept a typed intermediate asset (e.g. MaterialData list) and emit one
// batch ConversionResult. The orchestrator (FBXConverter) separates
// "single-file" converters (TextureConverter, ShaderConverter, ...) from
// "batch" converters (MaterialConverter, AnimationConverter, ...) by
// dynamic_cast<IConverterBatch*>(this), so the batch contract is opt-in.
//
// Rationale: previously `convertAll` was a per-converter free function
// declared on each concrete class with a different signature every time
// (e.g. MaterialConverter::convertAll(const vector<MaterialData>&, ...)
// vs. AnimationConverter::convertAll(const vector<AnimationData>&, ...)).
// Callers had to know the exact intermediate type up-front. This
// interface lets the orchestrator write
// `if (auto* batch = dynamic_cast<IConverterBatch*>(c)) batch->convertBatch(...)`
// without re-typing the per-converter signature.
class IConverterBatch {
public:
    virtual ~IConverterBatch() = default;
    // Run the converter against the typed intermediate asset. The
    // concrete implementation is responsible for the right cast: in
    // MaterialConverter etc. this is a thin wrapper around the existing
    // per-class `convertAll` overload.
    virtual std::vector<ConversionResult::ConvertedResource> convertBatch(
        const void* intermediate,
        const std::string& baseName) = 0;
};

} // namespace ayt::resource
