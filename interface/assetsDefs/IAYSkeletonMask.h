// IAYSkeletonMask.h — P3.x 刀 1 (2026-08-06) Skeleton Mask resource interface.
//
// Resource-level bone mask, gates per-bone TRS writes inside
// AnimationPlayer::evaluate() Phase 2 pre-lerp. Orthogonal to P1.5
// per-slot per-track `trackWeights` (which gates additive slot deltas
// keyed by clip track index). ISkeletonMask gates per-bone TRS writes
// keyed by SKELETON bone name, so the host can author a mask against
// the skeleton (e.g. "upper-body only") and have it apply across
// every clip bound to that skeleton without per-clip track-index
// knowledge.
//
// See AYAnimation/design.md §4.13 for full contract (INV-12..17).
//
// Migration note: This interface replaces the P2.2 temporary in-package
// `ayt::anim::ISkeletonMask` (AYAnimation/include/ayanimation/). It
// lives in `ayt::resource` namespace to match the convention of all 13
// other IResource subclasses (ISkeleton / IAnimation / IMesh / etc.).
// AnimationPlayer.h / AnimationSystem.cpp / SkeletonMaskBridge.cpp flip
// their include path + type ref from `ayt::anim::ISkeletonMask` to
// `ayt::resource::ISkeletonMask`.

#pragma once

#include "IAYResource.h"

#include <aymath/MathTypes.h>     // FGuid

#include <cstddef>
#include <cstdint>
#include <string>

namespace ayt::resource
{

// Per-bone mask record. POD; std::string name matches ayt::resource::Bone
// layout convention. `resolvedIndex` is populated by AnimationPlayer's
// resolver (AssetBoneCache); -1 = unresolved / not found. `isWildcard`
// is set by SkeletonMask::addEntry when the input boneName is empty;
// the resolver skips wildcard rows in the named pass.
struct SkeletonMaskBone {
    std::string  name;             // empty = wildcard row
    float        weight;           // [0, 1], pre-clamped by SkeletonMask::addEntry
    std::int32_t resolvedIndex;    // -1 = unresolved; >= 0 = bone index
    bool         isWildcard;       // true => apply to all bones not named elsewhere
};

class ISkeletonMask : public IResource {
public:
    virtual ~ISkeletonMask() = default;

    // Total entries the resolver sees: named entries + 1 if hasWildcard.
    virtual std::size_t getEntryCount() const = 0;

    // Pointer to the contiguous named-entry array. Wildcard is NOT in
    // this array; use hasWildcard() / wildcardWeight() to read it.
    virtual const SkeletonMaskBone* getEntries() const = 0;

    // Number of named entries (excluding any wildcard).
    virtual std::size_t getAuthoredBoneCount() const = 0;

    virtual bool  hasWildcard()    const = 0;
    virtual float wildcardWeight() const = 0;

    // Human-readable name for logs / debug overlays. Not used by the
    // resolver.
    virtual const char* getDebugName() const = 0;

    // GUID for L2 cache key — ResourceManager's persistent cache uses
    // GUID, so every concrete resource must expose this.
    virtual const ayt::math::FGuid& getGuid() const = 0;

    // Type tag / version — mirrors ISkeleton / IAnimation.
    static constexpr const char*        TYPE    = "SkeletonMask";
    static constexpr ayt::math::UInt32  VERSION = 1;
};

} // namespace ayt::resource