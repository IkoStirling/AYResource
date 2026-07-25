#pragma once
#include "IAYResource.h"
#include "aymath/MathTypes.h"
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

// ===== 动画通知 (Anim Notify marker) =====
//
// Phase 1.5 (2026-07-26) — P1.1 of AYAnimation design.md §14.
// Industrial-grade equivalent: Unreal FAnimNotifyEvent, Unity AnimationEvent,
// Godot 4 TYPE_METHOD track. Each marker is a time-keyed callback named
// ("OnFootstep", "OnHit", ...) optionally carrying a float payload.
//
// The marker list is sorted by `time` so AnimationPlayer can binary-search
// (or linear-scan, given notify counts per clip are 10–100) the range
// [prevTickTime, currentTime] each tick and fire crossings.
//
// Lifetime contract: `name` is owned by the IAnimation asset. AnimationPlayer
// exposes `const char*` back to subscribers via its sink / pendingNotifies /
// AnimNotifyEvent. Subscribers MUST NOT retain these pointers past the
// sync emit() callback boundary.
struct AnimNotifyMarker {
    std::string name;     // marker name (e.g. "OnFootstep", "OnHit")
    Float32     time    = 0.0f;   // seconds on the AYAnimation normalized timeline
    Float32     payload = 0.0f;   // optional float (e.g. SFX volume, damage)
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

    // ===== Anim Notify markers (Phase 1.5) =====
    //
    // Per-clip array of named, time-keyed events. The list is sorted
    // by `time` (loader invariant). Indices [0, getNotifyCount()).
    virtual UInt32         getNotifyCount()                       const = 0;
    virtual const char*    getNotifyName(UInt32 notifyIndex)      const = 0;
    virtual Float32        getNotifyTime(UInt32 notifyIndex)      const = 0;
    virtual Float32        getNotifyPayload(UInt32 notifyIndex)   const = 0;

    // ===== Constants =====
    // v1 (2026-06 baseline): tracks only.
    // v2 (2026-07-26, Phase 1.5): tracks + notify markers (this file).
    // Backward compat: v1 binaries skip the notify block on load; getNotifyCount()
    // returns 0; markers are simply absent.
    static constexpr UInt32 VERSION = 2;
    static constexpr UInt32 MAGIC = 0x4E4D5941; // 'AYNM'
};

} // namespace ayt::resource