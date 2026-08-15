#pragma once
#include "IAYResource.h"
#include "AYMath/MathTypes.h"
#include <cstdint>

namespace ayt::resource
{

// ===== 动画轨道类型 =====
enum class AnimTrackType : UInt8 {
    Vector3,    // 位置、缩放: 3 floats
    Quaternion,  // 旋转: 4 floats (x,y,z,w)
    Float,       // 单浮点数: 1 float
};

// ===== 动画轨道混合模式 (Phase 1.2 — P1.2 Layer 1 MVP) =====
//
// Industrial-grade equivalent: Unreal `UAnimSequence::bAdditive`,
// Unity `AnimationLayer.Additive`, Godot 4 AnimationTree mix node.
// v3 binary adds one byte per AnimTrack (between valueType and timeCount).
//
//   Override — same as v2: the sampled value REPLACES the bone's local TRS.
//              Default for any pre-v3 byte stream; default-initialized
//              AnimTrack structs get this value.
//
//   Additive — the sampled value is applied AS A DELTA on top of the bone's
//              base local TRS at evaluate-time. Math:
//                position: _localPos[k] += sample[k] * additiveWeight
//                rotation: result = (base * sample.pow(additiveWeight)).normalize()
//                          (weight==0 → identity early-return, base unchanged)
//                scale:    _localScl[k] *= (1.0f + sample[k] * additiveWeight)
//              additiveWeight is a per-player scalar 0..1 (saturated on write).
//              Float tracks always go to the host sink regardless of blend mode
//              (the additive concept is local-TRS only).
//
//   Assumption: an additive clip is AUTHORED with ref pose at t=0 — sample at
//   t=0 should be delta-zero relative to the bound skeleton rest pose. Document
//   in design.md §4.6. Authored-anywhere-at-t=0 additive clips drift; a
//   ref-pose capture path is deferred (P2.x).
enum class AnimBlendMode : UInt8 {
    Override = 0,  // default; bit-identical to v2 behavior
    Additive = 1,
};

// ===== 动画轨道数据 =====
struct AnimTrack {
    std::string nodeName;        // 目标节点/骨骼名称
    std::string property;        // 属性: "position", "rotation", "scale"
    AnimTrackType valueType;     // 值类型
    // Phase 1.2 (P1.2): per-track blend mode. Default-init = Override = v2
    // behavior. The byte is written by IAnimation::VERSION=3 saveToBinary;
    // v1/v2 loaders ignore it (no byte exists at this slot in their format).
    AnimBlendMode blendMode = AnimBlendMode::Override;

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

    // ===== Per-track blend mode (Phase 1.2 — P1.2 Layer 1 MVP) =====
    //
    // Override (0) = the same as v2: the sampled value REPLACES the bone's
    //               local TRS. Out-of-range trackIndex safely returns Override.
    // Additive (1) = the sampled value is a delta; AnimationPlayer applies
    //               it on top of the bone's base local TRS weighted by the
    //               player's additiveWeight scalar (0..1).
    virtual AnimBlendMode getTrackBlendMode(UInt32 trackIndex) const = 0;

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
    // v3 (2026-07-26, Phase 1.2 P1.2): tracks + per-track blendMode byte
    //     + notify markers. Every track record carries a 1-byte blendMode
    //     slot inserted between valueType and the timeCount UInt32.
    // v4 (2026-07-26, Phase 1.3 P1.3): reserved for Cross-Fade Layer 2
    //     additive-source semantics. **No on-disk change** — v4 writes
    //     identical bytes to v3 (same track layout, same notify block).
    //     The version bump locks in the AnimationPlayer-side dual-source
    //     API contract: from v4 onward, hosts may call setAdditiveSource()
    //     on a bound player to layer a second clip with discrete
    //     blendWeight. The on-disk format is unchanged so all v1/v2/v3
    //     .ayanm files round-trip identically through the v4 loader.
    // Backward compat:
    //   v1 binaries skip the notify block (getNotifyCount()==0) AND skip
    //       every track's blendMode byte (no byte exists at that slot).
    //   v2 binaries read the notify block but skip the blendMode byte
    //       (no byte in the v2 format).
    //   v3 binaries read both blocks.
    //   v4 binaries read both blocks (same as v3) — no extra bytes.
    // Forward compat:
    //   loadFromBinary returns false for any version > VERSION (5, 6, …)
    //   so a future binary that adds new bytes is caught loudly rather
    //   than silently mis-parsed.
    static constexpr UInt32 VERSION = 4;
    static constexpr UInt32 MAGIC = 0x4E4D5941; // 'AYNM'
};

} // namespace ayt::resource