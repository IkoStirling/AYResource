#pragma once
#include "AYResource/MaterialTextureContract.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <AYMath/MathTypes.h>
#include "AYResource/assetsDefs/IAnimation.h"  // for AnimTrackType

#define AYT_RESOURCE_STRINGIZE_IMPL(value) #value
#define AYT_RESOURCE_STRINGIZE(value) AYT_RESOURCE_STRINGIZE_IMPL(value)
#define AYT_RESOURCE_INTERMEDIATE_ASSET_ABI_VERSION 18

// IntermediateAsset crosses converter/module boundaries by value. Detect
// consumers built against an older field layout before they can corrupt
// memory. Keep this in sync with the importer contract revision when layout
// or enum representation changes.
#if defined(_MSC_VER)
#pragma detect_mismatch("AYResource.IntermediateAsset.ABI", AYT_RESOURCE_STRINGIZE(AYT_RESOURCE_INTERMEDIATE_ASSET_ABI_VERSION))
#endif

namespace ayt::resource
{
using namespace ayt::math;

// ===== 纹理格式 =====
enum class TextureFormat : UInt8 {
    RGBA8 = 0,
    RGB8 = 1,
    BC1 = 2,   // DXT1
    BC3 = 3,   // DXT5 / BC3
    BC4 = 4,   // ATI1
    BC5 = 5,   // ATI2
    BC7 = 6,   // 更高质量
    Unknown = 255
};

// ===== 参数类型 =====
enum class MaterialParamType : UInt8 {
    Float = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    Float4x4 = 4,  // Matrix 4x4
    Texture2D = 5,
    Texture3D = 6,
    TextureCube = 7,
    Int = 8,
    Bool = 9,
    String = 10
};

// Surface routing is material metadata, not a shader parameter.  Keeping it
// typed here lets every source converter produce the same contract and keeps
// AYRenderer from inferring pass selection from texture pixels.
enum class MaterialAlphaMode : UInt8 {
    Opaque = 0,
    Mask = 1,
    Blend = 2
};

// Importer-neutral material state. Source adapters (Assimp today, FBX SDK in
// the future) translate their native enums into this contract before cooking.
// AlphaMode answers which render route is used; BlendFunction answers how a
// transparent draw is composited. Keeping them separate avoids collapsing an
// authored additive material into ordinary alpha blending.
enum class MaterialBlendFunction : UInt8 {
    StandardAlpha = 0,
    Additive = 1
};

enum class MaterialShadingModel : UInt8 {
    Unknown = 0,
    Flat,
    Gouraud,
    Phong,
    Blinn,
    Toon,
    OrenNayar,
    Minnaert,
    CookTorrance,
    Unlit,
    Fresnel,
    Pbr
};

enum class MaterialTextureMapping : UInt8 {
    Uv = 0,
    Sphere,
    Cylinder,
    Box,
    Plane,
    Other
};

enum class MaterialTextureOperation : UInt8 {
    Multiply = 0,
    Add,
    Subtract,
    Divide,
    SmoothAdd,
    SignedAdd
};

enum class MaterialTextureWrap : UInt8 {
    Wrap = 0,
    Clamp,
    Mirror,
    Decal
};

enum class MaterialSourcePropertyType : UInt8 {
    Float = 0,
    Double,
    String,
    Integer,
    Buffer
};

enum class MaterialSurfaceSource : UInt8 {
    Default = 0,
    ExplicitSource = 1,
    TextureCoverage = 2,
    // Kept as an alias for source compatibility with older callers.  The
    // importer no longer applies model/name compatibility rules.
    CompatibilityRule = TextureCoverage,
    ConfigOverride = 3
};

// ===== 中间数据结构 =====

struct Param {
    std::string name;
    MaterialParamType type;
    // Keep the intermediate representation trivially safe to copy.  FBX
    // parameters are moved through vectors before MaterialConverter stores
    // them in an unordered_map; an anonymous union beside std::string makes
    // that path unnecessarily fragile in MSVC debug builds.
    float floatValue = 0.0f;
    float float2Value[2] = {0.0f, 0.0f};
    float float3Value[3] = {0.0f, 0.0f, 0.0f};
    float float4Value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float matrixValue[16] = {};
    int intValue = 0;
    bool boolValue = false;
    std::string texturePath; // for Texture2D/3D/Cube
    std::string stringValue;
};

struct SubmeshData {
    uint32_t startIndex = 0;      // index offset in index buffer
    uint32_t indexCount = 0;
    uint32_t vertexOffset = 0;
    uint32_t materialIndex = 0;
    // Index into IntermediateAsset::materials before materialSlots are
    // flattened into runtime virtual paths.  This is importer-only metadata:
    // MeshConverter intentionally does not serialize it into .aymesh.
    uint32_t sourceMaterialIndex = UINT32_MAX;
    // Cooked skin sections own a compact draw-local palette. Each entry maps
    // the UInt32 local joint stored on a vertex back to SkeletonData::bones.
    // Source/import meshes leave this empty and use GlobalSkeleton indices.
    std::vector<UInt32> bonePalette;
};

enum class SkinIndexSpace : UInt8 {
    GlobalSkeleton = 0,
    LocalPalette = 1
};

struct SkinVertexData {
    UInt32 joint[4] = {0u, 0u, 0u, 0u};
    Float32 weight[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct MorphVertexDelta {
    UInt32 vertexIndex = 0;
    Float32 positionDelta[3] = {0.0f, 0.0f, 0.0f};
    Float32 normalDelta[3] = {0.0f, 0.0f, 0.0f};
    Float32 tangentDelta[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct MorphTargetData {
    std::string name;
    // Neutral/base-mesh weight. Source-format "full weight" thresholds must
    // not be stored here; runtime starts from this value before animation.
    Float32 defaultWeight = 0.0f;
    std::vector<MorphVertexDelta> deltas;
};

struct MeshData {
    std::string name;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<float> tangents;
    std::vector<float> colors;
    std::vector<uint32_t> indices;
    std::vector<SubmeshData> submeshes;
    std::vector<std::string> materialSlots;
    std::vector<MorphTargetData> morphTargets;
    uint8_t attributeMask = 0;
    float boundsMin[3] = {0};
    float boundsMax[3] = {0};
    // Canonical converter representation. Imported data keeps scene-wide
    // SkeletonData indices as UInt32 until the platform cooking step builds
    // per-draw palettes. This prevents silent truncation on large skeletons.
    std::vector<SkinVertexData> skinVertices;
    SkinIndexSpace skinIndexSpace = SkinIndexSpace::GlobalSkeleton;
};

struct MaterialData {
    struct SourceProperty {
        std::string key;
        UInt32 semantic = 0;
        UInt32 index = 0;
        MaterialSourcePropertyType type = MaterialSourcePropertyType::Buffer;
        std::string value;
    };

    std::string sourceAdapter;
    std::string name;
    std::string shader;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    MaterialBlendFunction blendFunction = MaterialBlendFunction::StandardAlpha;
    MaterialShadingModel shadingModel = MaterialShadingModel::Unknown;
    MaterialSurfaceSource surfaceSource = MaterialSurfaceSource::Default;
    std::vector<Param> parameters;
    // Lossless textual projection of every source-adapter property, including
    // vendor-specific entries not represented by the canonical PBR contract.
    // MaterialConverter stores this as one reserved string parameter.
    std::vector<SourceProperty> sourceProperties;
    // 原始纹理路径（用于 Converter 查找源文件）
    std::vector<std::string> texturePaths;
    struct TextureSource {
        std::string parameterName;
        std::string sourcePath;
        std::string virtualPath;
        std::string usageSuffix;
        TextureColorSpace colorSpace = TextureColorSpace::Linear;
        NormalMapY normalY = NormalMapY::Positive;
        // Complete source binding metadata. The first layer for a semantic
        // uses the canonical shader parameter name; additional layers remain
        // available under deterministic LayerN names for tools/custom shaders.
        UInt32 layerIndex = 0;
        UInt32 sourceSemantic = 0;
        UInt32 sourceLayer = 0;
        UInt32 uvChannel = 0;
        MaterialTextureMapping mapping = MaterialTextureMapping::Uv;
        MaterialTextureOperation operation = MaterialTextureOperation::Multiply;
        MaterialTextureWrap wrapU = MaterialTextureWrap::Wrap;
        MaterialTextureWrap wrapV = MaterialTextureWrap::Wrap;
        MaterialTextureWrap wrapW = MaterialTextureWrap::Wrap;
        Float32 blendFactor = 1.0f;
        Float32 uvTranslation[2] = {0.0f, 0.0f};
        Float32 uvScale[2] = {1.0f, 1.0f};
        Float32 uvRotation = 0.0f;
        UInt32 flags = 0;
        bool hasUvTransform = false;
    };
    // Semantic-preserving source bindings.  texturePaths remains for old
    // producers/tests; FBXConverter prefers this record so different slots
    // cannot overwrite each other at textures/{stem}_d.*.
    std::vector<TextureSource> textureSources;
};

struct TextureData {
    std::string name;
    int width = 0;
    int height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    std::vector<uint8_t> imageData;
    std::string usage; // "_d", "_n", "_s", etc.
};

struct KeyframeTrack {
    std::string targetNode; // bone/node name
    std::string property;  // "position", "rotation", "scale", "morph:<target>"
    AnimTrackType valueType = AnimTrackType::Vector3; // R-02: 透传到 IAnimation::AnimTrack
    // Phase 1.2 (P1.2): per-track blend mode passthrough. Default = Override;
    // an FBX take authored as additive marks this in the converter.
    AnimBlendMode blendMode = AnimBlendMode::Override;
    std::vector<float> times;
    std::vector<float> values;
};

// Phase 1.5: anim notify marker on the intermediate asset. Mirror of
// ayt::resource::AnimNotifyMarker (AYResource/assetsDefs/IAnimation.h). Currently populated only
// by the converter when a non-empty `notifies` is supplied via AnimationData;
// FBX has no first-class notify channel — see FBXParser::_parseAnimations
// TODO note.
struct AnimNotifyMarkerData {
    std::string name;     // e.g. "OnFootstep", "OnHit"
    float       time    = 0.0f;  // seconds on AYAnimation timeline
    float       payload = 0.0f;  // optional float (SFX volume, damage, ...)
};

struct AnimationData {
    std::string name;
    // Duration is seconds; KeyframeTrack::times are source ticks.
    float duration = 0.0f;
    float ticksPerSecond = 30.0f;
    std::vector<KeyframeTrack> tracks;
    // Phase 1.5: optional trailing notify list. Empty for clips authored
    // without notifies (e.g. FBX takes imported without metadata).
    std::vector<AnimNotifyMarkerData> notifies;
};

struct AudioData {
    std::string name;
    int sampleRate = 44100;
    int channels = 1;
    int bitsPerSample = 16;
    std::vector<uint8_t> audioData; // raw PCM data
};

struct ShaderData {
    std::string name;
    std::string source;       // GLSL/HLSL 源码
    std::string entryPoint;    // 入口点，如 "main"
    std::string profile;       // 着色器配置，如 "vs_5_0", "glsl_150"
    std::string usage;         // 用途标记，如 "_d", "_n", "_s"
};

struct GlyphData {
    UInt32 codepoint = 0;
    float u = 0.0f, v = 0.0f;      // UV 起始位置
    float w = 0.0f, h = 0.0f;     // 宽高
    float advance = 0.0f;           // 前进量
};

struct FontData {
    std::string name;
    UInt32 fontSize = 16;
    std::vector<GlyphData> glyphs;
    std::vector<UInt8> atlasData;  // RGBA8 纹理数据
};

struct VideoData {
    std::string name;
    int width = 0;
    int height = 0;
    float duration = 0.0f;
    float frameRate = 30.0f;
    std::vector<UInt8> frameData;  // 扁平化的 RGBA 帧数据
};

struct BoneData {
    std::string name;
    int parentIndex = -1;  // -1 表示根骨骼
    ayt::math::Float4x4 inverseBindMatrix;
    // R-02: 节点本地 rest pose (FBX aiNode::mTransformation 分解)
    // 当 FBX 文件中包含骨骼时,由 FBXParser 填充;若 FBX 不带骨骼,保持默认值。
    FVector3 localPosition = FVector3(0, 0, 0);
    FQuaternion localRotation = FQuaternion::identity();
    FVector3 localScale = FVector3(1, 1, 1);
};

struct SkeletonData {
    std::string name;
    std::vector<BoneData> bones;
};

// ===== 中间资产 =====
struct IntermediateAsset {
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<TextureData> textures;
    std::vector<AnimationData> animations;
    std::vector<AudioData> audios;
    std::vector<ShaderData> shaders;
    std::vector<VideoData> videos;
    std::vector<SkeletonData> skeletons;

    bool empty() const {
        return meshes.empty() && materials.empty() && textures.empty() &&
               animations.empty() && audios.empty() && shaders.empty() && videos.empty() && skeletons.empty();
    }
};

// ===== Parser 接口 =====
class IFormatParser {
public:
    virtual ~IFormatParser() = default;
    virtual bool parse(const std::string& sourcePath = "") = 0;
    virtual std::unique_ptr<IntermediateAsset> getResult() = 0;
    virtual const char* getFormatName() const = 0;
};

} // namespace ayt::resource

#undef AYT_RESOURCE_INTERMEDIATE_ASSET_ABI_VERSION
#undef AYT_RESOURCE_STRINGIZE
#undef AYT_RESOURCE_STRINGIZE_IMPL
