#pragma once
#include "IAYResource.h"
#include "IAYMaterial.h"
#include "IAYResourceLoader.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace ayt::resource
{

// ===== Material — IMaterial 实现类 =====
class Material : public IMaterial {
    friend class MaterialConverter;

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

    template<typename Fn>
    void forEachParameter(Fn&& fn) const
    {
        for (const auto& entry : _params) {
            fn(entry.first.c_str(), entry.second.type);
        }
    }

private:
    void clear();

    FGuid _guid;  // 资源唯一标识

    // ===== Parameter storage =====
    struct ParameterValue {
        MaterialParamType type;
        union {
            Float32 floatValue;
            Float32 float2Value[2];
            Float32 float3Value[3];
            Float32 float4Value[4];
            Int32 intValue;
            Bool boolValue;
            Float32 matrixValue[16]; // 4x4 matrix
        };
        std::string stringValue; // for texture paths
    };

    std::unordered_map<std::string, ParameterValue> _params;

    // ===== Basic info =====
    std::string _name;
    std::string _shader;

    // ===== Path =====
    std::string _path;
};

} // namespace ayt::resource