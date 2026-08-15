#pragma once
#include "AYResource/assetsDefs/IPhysics.h"
#include <AYMath/MathTypes.h>
#include <string>
#include <vector>
#include <array>

namespace ayt::resource
{

// ===== Physics — 物理资源实现类 =====
class Physics : public IPhysics {
public:
    Physics();
    virtual ~Physics() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    // ===== IAYPhysics =====
    const char* getName() const override { return _name.c_str(); }
    const char* getShapeType() const override;
    float getMass() const override { return _mass; }
    void setMass(float mass) override { _mass = mass; }

    PhysicsShapeType getShapeTypeEnum() const override { return _shapeType; }
    const float* getShapeData() const override { return _shapeData.data(); }
    UInt32 getShapeDataSize() const override { return static_cast<UInt32>(_shapeData.size() * sizeof(float)); }

    void getBoxHalfExtents(float& x, float& y, float& z) const override;
    void getSphereParams(float& radius, float& cx, float& cy, float& cz) const override;

    UInt32 getVertexCount() const override;
    const float* getVertexData() const override;

    // ===== 二进制序列化 =====
    bool loadFromBinary(const void* data, size_t size) override;
    bool saveToBinary(std::vector<UInt8>& outData) const override;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== 创建测试数据 =====
    void createBox(float hx, float hy, float hz);
    void createSphere(float radius);
    void createConvex(const float* vertices, UInt32 vertexCount);

private:
    void clear();
    static const char* shapeTypeToString(PhysicsShapeType type);

    FGuid _guid;  // 资源唯一标识

    // ===== 基本属性 =====
    std::string _name;
    float _mass = 1.0f;
    PhysicsShapeType _shapeType = PhysicsShapeType::Box;

    // ===== 形状数据 =====
    // Box: halfExtents[3]
    // Sphere: radius, center[3]
    // Convex/Mesh: float[vertexCount * 3]
    std::vector<float> _shapeData;

    // ===== 辅助数据 =====
    std::string _shapeTypeStr; // "box", "sphere", "mesh", "convex"
};

} // namespace ayt::resource