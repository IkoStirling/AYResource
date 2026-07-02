#pragma once
#include "IAYResource.h"
#include "AYMathTypes.h"

namespace ayt::resource
{

// ===== Bone — 骨骼数据结构 =====
struct Bone {
    std::string name;                    // 骨骼名称
    int parentIndex;                     // 父骨骼索引，-1 表示根骨骼

    // 逆绑定矩阵 (绑定姿势的逆矩阵)
    ayt::math::Float4x4 inverseBindMatrix;

    // 局部变换 (相对于父骨骼)
    ayt::math::FVector3 localPosition;      // 局部位置
    ayt::math::FQuaternion localRotation;   // 局部旋转
    ayt::math::FVector3 localScale;          // 局部缩放
};

// ===== ISkeleton — 骨骼资源接口 =====
class ISkeleton : public IResource {
public:
    virtual ~ISkeleton() = default;

    // ===== Bone data =====
    virtual size_t getBoneCount() const = 0;
    virtual const Bone* getBones() const = 0;

    // ===== Lookup =====
    virtual int findBone(const char* name) const = 0;
    virtual int getParentBoneIndex(size_t boneIndex) const = 0;

    // ===== Bind pose =====
    virtual const ayt::math::Float4x4* getInverseBindMatrices() const = 0;

    // ===== Local transforms =====
    virtual const ayt::math::FVector3* getLocalPositions() const = 0;
    virtual const ayt::math::FQuaternion* getLocalRotations() const = 0;
    virtual const ayt::math::FVector3* getLocalScales() const = 0;

    // ===== Hierarchy =====
    virtual size_t getRootBoneCount() const = 0;
    virtual const int* getRootBoneIndices() const = 0;

    // ===== Constants =====
    static constexpr const char* TYPE = "Skeleton";
    static constexpr UInt32 VERSION = 1;
};

} // namespace ayt::resource