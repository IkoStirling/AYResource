#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <AYMath/MathTypes.h>
#include "AYResource/assetsDefs/IAnimation.h"  // for AnimTrackType

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
    Bool = 9
};

// ===== 中间数据结构 =====

struct Param {
    std::string name;
    MaterialParamType type;
    union {
        float floatValue;
        float float2Value[2];
        float float3Value[3];
        float float4Value[4];
        float matrixValue[16]; // Float4x4
        int intValue;
        bool boolValue;
    };
    std::string texturePath; // for Texture2D/3D/Cube
};

struct SubmeshData {
    uint32_t startIndex = 0;      // index offset in index buffer
    uint32_t indexCount = 0;
    uint32_t vertexOffset = 0;
    uint32_t materialIndex = 0;
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
    uint8_t attributeMask = 0;
    float boundsMin[3] = {0};
    float boundsMax[3] = {0};
    // 骨骼蒙皮数据：每顶点 8 floats (4 bone indices as float + 4 weights)
    // bone indices 存储为 float（避免类型混用），实际使用时会转换回 UInt8
    std::vector<float> skinWeights;
};

struct MaterialData {
    std::string name;
    std::string shader;
    std::vector<Param> parameters;
    // 原始纹理路径（用于 Converter 查找源文件）
    std::vector<std::string> texturePaths;
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
    std::string property;  // "position", "rotation", "scale"
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
