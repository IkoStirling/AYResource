#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <AYMath/MathTypes.h>

namespace ayt::resource
{

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
