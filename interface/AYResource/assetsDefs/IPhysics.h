#pragma once
#include "AYResource/IResource.h"
#include <memory>

namespace ayt::resource
{

// ===== 形状类型枚举 =====
enum class PhysicsShapeType : UInt8 {
    Box     = 0,  // 盒体: halfExtents[3]
    Sphere  = 1,  // 球体: radius + center[3]
    Mesh    = 2,  // 三角网格: 顶点数据
    Convex  = 3,  // 凸包: 顶点数据
};

// ===== IPhysics — 物理资源接口 =====
class IPhysics : public IResource {
public:
    virtual ~IPhysics() = default;

    // ===== 基本信息 =====
    virtual const char* getName() const = 0;
    virtual const char* getShapeType() const = 0; // "box", "sphere", "mesh", "convex"

    // ===== 物理属性 =====
    virtual float getMass() const = 0;
    virtual void setMass(float mass) = 0;

    // ===== 形状数据 =====
    virtual PhysicsShapeType getShapeTypeEnum() const = 0;
    virtual const float* getShapeData() const = 0; // 形状参数指针
    virtual UInt32 getShapeDataSize() const = 0;    // shapeData 字节数

    // ===== Box 专用访问 =====
    virtual void getBoxHalfExtents(float& x, float& y, float& z) const = 0;

    // ===== Sphere 专用访问 =====
    virtual void getSphereParams(float& radius, float& cx, float& cy, float& cz) const = 0;

    // ===== Mesh/Convex 专用访问 =====
    virtual UInt32 getVertexCount() const = 0;
    virtual const float* getVertexData() const = 0; // float[vertexCount * 3]

    // ===== 二进制序列化 =====
    virtual bool loadFromBinary(const void* data, size_t size) = 0;
    virtual bool saveToBinary(std::vector<UInt8>& outData) const = 0;

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x48595041; // 'AYPH' in little-endian (0x41=P, 0x59=Y, 0x48=H)
};

} // namespace ayt::resource