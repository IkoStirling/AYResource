#pragma once
#include "AYResource/IResource.h"
#include "AYResource/assetsDefs/IMaterial.h"
#include "AYResource/IResourceLoader.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace ayt::resource
{

// ===== Material — IMaterial 实现类 =====
class Material : public IMaterial {
    // F2.1: removed `friend class MaterialConverter;`. The converter now
    // uses `forEachParameterHashSink(...)` (defined inline below) to walk
    // `_params` for the F1.5b content-hash without touching private fields
    // directly.

public:
    Material();
    virtual ~Material() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    // ===== IMaterial =====
    const char* getName() const override { return _name.c_str(); }
    const char* getShader() const override { return _shader.c_str(); }
    MaterialAlphaMode getAlphaMode() const override { return _alphaMode; }
    Float32 getAlphaCutoff() const override { return _alphaCutoff; }
    Bool isDoubleSided() const override { return _doubleSided; }

    UInt32 getParameterCount() const override { return static_cast<UInt32>(_params.size()); }
    bool hasParameter(const char* name) const override;
    MaterialParamType getParameterType(const char* name) const override;

    // ===== Scalar types =====
    Float32 getFloat(const char* name) const override;
    Int32 getInt(const char* name) const override;
    Bool getBool(const char* name) const override;

    // ===== Vector types (AYMath) =====
    ayt::math::FVector2 getVector2(const char* name) const override;
    ayt::math::FVector3 getVector3(const char* name) const override;
    ayt::math::FVector4 getVector4(const char* name) const override;

    // ===== Color (FVector4 alias) =====
    ayt::math::FVector4 getColor(const char* name) const override;

    // ===== Matrix =====
    const Float32* getMatrix(const char* name) const override;

    // ===== Legacy raw access =====
    void getFloat2(const char* name, Float32(&out)[2]) const override;
    void getFloat3(const char* name, Float32(&out)[3]) const override;
    void getFloat4(const char* name, Float32(&out)[4]) const override;

    // ===== Texture =====
    const char* getTexture(const char* name) const override;

    // ===== Setters =====
    void setName(const std::string& name) { _name = name; }
    void setShader(const std::string& shader) { _shader = shader; }
    void setSurfaceProperties(MaterialAlphaMode alphaMode, Float32 alphaCutoff,
                              Bool doubleSided) {
        _alphaMode = alphaMode;
        _alphaCutoff = alphaCutoff;
        _doubleSided = doubleSided;
    }

    void setFloat(const char* name, Float32 value);
    void setInt(const char* name, Int32 value);
    void setBool(const char* name, Bool value);

    void setVector2(const char* name, const ayt::math::FVector2& value);
    void setVector3(const char* name, const ayt::math::FVector3& value);
    void setVector4(const char* name, const ayt::math::FVector4& value);
    void setColor(const char* name, const ayt::math::FVector4& value);
    void setMatrix(const char* name, const Float32* value);

    // Legacy
    void setFloat2(const char* name, const Float32 value[2]);
    void setFloat3(const char* name, const Float32 value[3]);
    void setFloat4(const char* name, const Float32 value[4]);
    void setTexture(const char* name, const char* path);

    // ===== Binary serialization =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // 多材质支持：保存/加载单材质数据段（无文件头）
    bool saveToMaterialData(std::vector<UInt8>& outData) const;
    bool loadFromMaterialData(const void* data, size_t size);
    size_t getMaterialDataSize() const;

    // ===== 创建测试材质 =====
    void createDefault();

    // F2.1: replace friend access for converter reset paths.
    void clear() { clearImpl(); }
    // Initialise a fresh Material with a name + shader (replaces the
    // converter's old pattern of `material->clear(); setName(); setShader();`).
    void initialise(const std::string& name, const std::string& shader) {
        clearImpl();
        _name = name;
        _shader = shader;
    }

    template<typename Fn>
    void forEachParameter(Fn&& fn) const
    {
        for (const auto& entry : _params) {
            fn(entry.first.c_str(), entry.second.type);
        }
    }

    // F2.1: serialize all parameter values into a binary buffer usable
    // for content-hashing (the F1.5b MaterialConverter fix). The callback
    // is invoked once per parameter in iteration order with the type tag
    // and a byte span of the parameter's value storage. The caller can
    // stream the bytes into a SHA-256 / FGuid accumulator without ever
    // reaching into the private `_params` map.
    template<typename Fn>
    void forEachParameterHashSink(Fn&& fn) const
    {
        for (const auto& entry : _params) {
            const auto& pv = entry.second;
            const std::string& name = entry.first;
            switch (pv.type) {
            case MaterialParamType::Float:
                fn(name, pv.type,
                   reinterpret_cast<const UInt8*>(&pv.floatValue),
                   sizeof(Float32));
                break;
            case MaterialParamType::Float2:
                fn(name, pv.type,
                   reinterpret_cast<const UInt8*>(pv.float2Value),
                   sizeof(Float32) * 2);
                break;
            case MaterialParamType::Float3:
                fn(name, pv.type,
                   reinterpret_cast<const UInt8*>(pv.float3Value),
                   sizeof(Float32) * 3);
                break;
            case MaterialParamType::Float4:
                fn(name, pv.type,
                   reinterpret_cast<const UInt8*>(pv.float4Value),
                   sizeof(Float32) * 4);
                break;
            case MaterialParamType::Float4x4:
                fn(name, pv.type,
                   reinterpret_cast<const UInt8*>(pv.matrixValue),
                   sizeof(Float32) * 16);
                break;
            case MaterialParamType::Int:
                fn(name, pv.type,
                   reinterpret_cast<const UInt8*>(&pv.intValue),
                   sizeof(Int32));
                break;
            case MaterialParamType::Bool:
                fn(name, pv.type,
                   reinterpret_cast<const UInt8*>(&pv.boolValue),
                   sizeof(Bool));
                break;
            case MaterialParamType::Texture2D:
            case MaterialParamType::Texture3D:
            case MaterialParamType::TextureCube:
                fn(name, pv.type,
                   reinterpret_cast<const UInt8*>(pv.stringValue.data()),
                   pv.stringValue.size());
                break;
            }
        }
    }

private:
    void clearImpl();

    FGuid _guid;  // 资源唯一标识

    // ===== Parameter storage =====
    struct ParameterValue {
        MaterialParamType type = MaterialParamType::Float;
        // These fields intentionally are not a union.  A union placed next
        // to std::string has no active-member tracking and is very easy to
        // copy incorrectly through unordered_map/debug-iterator machinery.
        // The extra storage is negligible compared with a material and
        // keeps every value fully initialized and independently copyable.
        Float32 floatValue = 0.0f;
        Float32 float2Value[2] = {0.0f, 0.0f};
        Float32 float3Value[3] = {0.0f, 0.0f, 0.0f};
        Float32 float4Value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Int32 intValue = 0;
        Bool boolValue = false;
        Float32 matrixValue[16] = {}; // 4x4 matrix
        std::string stringValue; // for texture paths

        // Keep the string member's lifetime independent from the numeric
        // union.  Material values are copied by unordered_map when a
        // parameter is inserted; explicitly defaulting the special members
        // makes that operation well-defined even in MSVC debug iterator mode.
        ParameterValue() = default;
        ParameterValue(const ParameterValue&) = default;
        ParameterValue& operator=(const ParameterValue&) = default;
        ~ParameterValue() = default;
    };

    std::unordered_map<std::string, ParameterValue> _params;

    // ===== Basic info =====
    std::string _name;
    std::string _shader;
    MaterialAlphaMode _alphaMode = MaterialAlphaMode::Opaque;
    Float32 _alphaCutoff = 0.5f;
    Bool _doubleSided = false;
    // Legacy MaterialFile segments had no per-material header. Keep the
    // encoding version so getMaterialDataSize() reports the bytes actually
    // consumed when reading one of those old multi-material packs.
    UInt8 _materialDataEncodingVersion = 2;

    // ===== Path =====
    std::string _path;
};

} // namespace ayt::resource
