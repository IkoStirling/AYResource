#pragma once
#include "AYResource/IResource.h"
#include "AYResource/IntermediateAsset.h"
#include "AYMath/MathTypes.h"
#include <cstdint>

namespace ayt::resource
{

// ===== IMaterial — 材质资源接口 =====
class IMaterial : public IResource {
public:
    virtual ~IMaterial() = default;

    // ===== Properties =====
    virtual const char* getName() const = 0;
    virtual const char* getShader() const = 0;

    // ===== Parameters =====
    virtual UInt32 getParameterCount() const = 0;
    virtual bool hasParameter(const char* name) const = 0;
    virtual MaterialParamType getParameterType(const char* name) const = 0;

    // ===== Scalar types =====
    virtual Float32 getFloat(const char* name) const = 0;
    virtual Int32 getInt(const char* name) const = 0;
    virtual Bool getBool(const char* name) const = 0;

    // ===== Vector types (AYMath) =====
    virtual ayt::math::FVector2 getVector2(const char* name) const = 0;
    virtual ayt::math::FVector3 getVector3(const char* name) const = 0;
    virtual ayt::math::FVector4 getVector4(const char* name) const = 0;

    // ===== Color (FVector4 alias, RGBA) =====
    virtual ayt::math::FVector4 getColor(const char* name) const = 0;

    // ===== Matrix (Float32[16], column-major) =====
    virtual const Float32* getMatrix(const char* name) const = 0; // 4x4 matrix

    // ===== Legacy raw access (兼容) =====
    virtual void getFloat2(const char* name, Float32(&out)[2]) const = 0;
    virtual void getFloat3(const char* name, Float32(&out)[3]) const = 0;
    virtual void getFloat4(const char* name, Float32(&out)[4]) const = 0;

    // ===== Texture =====
    virtual const char* getTexture(const char* name) const = 0;

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x544D5941; // 'AYMT'
};

} // namespace ayt::resource