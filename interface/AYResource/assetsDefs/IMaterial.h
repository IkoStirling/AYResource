#pragma once
#include "AYResource/IResource.h"
#include "AYResource/IntermediateAsset.h"
#include "AYMath/MathTypes.h"
#include <cstdint>

#define AYT_RESOURCE_STRINGIZE_IMPL(value) #value
#define AYT_RESOURCE_STRINGIZE(value) AYT_RESOURCE_STRINGIZE_IMPL(value)
#define AYT_RESOURCE_IMATERIAL_ABI_VERSION 3

// A stale consumer of this polymorphic interface is memory-unsafe. Make MSVC
// reject mixed object files at link time instead of allowing a vtable mismatch
// to surface later as an access violation. Bump this whenever the class ABI
// changes (virtual functions, bases, data layout, or calling convention).
#if defined(_MSC_VER)
#pragma detect_mismatch("AYResource.IMaterial.ABI", AYT_RESOURCE_STRINGIZE(AYT_RESOURCE_IMATERIAL_ABI_VERSION))
#endif

namespace ayt::resource
{

// ===== IMaterial — 材质资源接口 =====
class IMaterial : public IResource {
public:
    virtual ~IMaterial() = default;

    // ===== Properties =====
    virtual const char* getName() const = 0;
    virtual const char* getShader() const = 0;
    virtual MaterialAlphaMode getAlphaMode() const = 0;
    virtual Float32 getAlphaCutoff() const = 0;
    virtual Bool isDoubleSided() const = 0;

    // ===== Parameters =====
    virtual UInt32 getParameterCount() const = 0;
    virtual bool hasParameter(const char* name) const = 0;
    virtual MaterialParamType getParameterType(const char* name) const = 0;

    // ===== Scalar types =====
    virtual Float32 getFloat(const char* name) const = 0;
    virtual Int32 getInt(const char* name) const = 0;
    virtual Bool getBool(const char* name) const = 0;
    virtual const char* getString(const char* name) const = 0;

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
    // v2 promotes surface routing out of magic shader parameters. v3 adds a
    // String parameter payload for lossless source-import metadata. The
    // loader remains backward compatible with v1/v2 assets.
    static constexpr UInt32 ABI_VERSION = AYT_RESOURCE_IMATERIAL_ABI_VERSION;
    static constexpr UInt32 VERSION = 3;
    static constexpr UInt32 MAGIC = 0x544D5941; // 'AYMT'
};

} // namespace ayt::resource

#undef AYT_RESOURCE_IMATERIAL_ABI_VERSION
#undef AYT_RESOURCE_STRINGIZE
#undef AYT_RESOURCE_STRINGIZE_IMPL
