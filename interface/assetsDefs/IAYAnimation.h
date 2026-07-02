#pragma once
#include "IAYResource.h"
#include "AYMathTypes.h"
#include <cstdint>

namespace ayt::resource
{

// ===== 动画轨道类型 =====
enum class AnimTrackType : UInt8 {
    Vector3,    // 位置、缩放: 3 floats
    Quaternion,  // 旋转: 4 floats (x,y,z,w)
    Float,       // 单浮点数: 1 float
};

// ===== 动画轨道数据 =====
struct AnimTrack {
    std::string nodeName;        // 目标节点/骨骼名称
    std::string property;        // 属性: "position", "rotation", "scale"
    AnimTrackType valueType;     // 值类型

    std::vector<Float32> times;  // 时间点
    std::vector<Float32> values; // 值 (根据 valueType 解释)
};

// ===== IAnimation — 动画资源接口 =====
class IAnimation : public IResource {
public:
    virtual ~IAnimation() = default;

    // ===== Basic info =====
    virtual const char* getName() const = 0;
    virtual Float32 getDuration() const = 0;
    virtual Float32 getTicksPerSecond() const = 0;

    // ===== Tracks =====
    virtual UInt32 getTrackCount() const = 0;
    virtual const char* getTrackNodeName(UInt32 trackIndex) const = 0;
    virtual const char* getTrackProperty(UInt32 trackIndex) const = 0;
    virtual AnimTrackType getTrackType(UInt32 trackIndex) const = 0;
    virtual UInt32 getTrackKeyframeCount(UInt32 trackIndex) const = 0;
    virtual const Float32* getTrackTimes(UInt32 trackIndex) const = 0;

    // ===== 值访问 (根据类型返回) =====
    virtual const ayt::math::FVector3* getTrackVector3Values(UInt32 trackIndex) const = 0;
    virtual const ayt::math::FQuaternion* getTrackQuaternionValues(UInt32 trackIndex) const = 0;
    virtual const Float32* getTrackFloatValues(UInt32 trackIndex) const = 0;

    // ===== Legacy raw values (兼容) =====
    virtual const Float32* getTrackValues(UInt32 trackIndex) const = 0;

    // ===== Constants =====
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x4E4D5941; // 'AYNM'
};

} // namespace ayt::resource