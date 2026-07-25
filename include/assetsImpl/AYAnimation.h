#pragma once
#include "IAYAnimation.h"
#include <aymath/MathTypes.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace ayt::resource
{

// ===== Animation — IAnimation 实现类 =====
class Animation : public IAnimation {
public:
    Animation();
    virtual ~Animation() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    // ===== IAnimation =====
    const char* getName() const override { return _name.c_str(); }
    Float32 getDuration() const override { return _duration; }
    Float32 getTicksPerSecond() const override { return _ticksPerSecond; }

    UInt32 getTrackCount() const override { return static_cast<UInt32>(_tracks.size()); }
    const char* getTrackNodeName(UInt32 trackIndex) const override;
    const char* getTrackProperty(UInt32 trackIndex) const override;
    AnimTrackType getTrackType(UInt32 trackIndex) const override;
    UInt32 getTrackKeyframeCount(UInt32 trackIndex) const override;
    const Float32* getTrackTimes(UInt32 trackIndex) const override;

    // ===== 值访问 =====
    const ayt::math::FVector3* getTrackVector3Values(UInt32 trackIndex) const override;
    const ayt::math::FQuaternion* getTrackQuaternionValues(UInt32 trackIndex) const override;
    const Float32* getTrackFloatValues(UInt32 trackIndex) const override;

    // ===== Legacy =====
    const Float32* getTrackValues(UInt32 trackIndex) const override;

    // ===== Anim Notify markers (Phase 1.5) — IAnimation overrides =====
    UInt32      getNotifyCount()                     const override;
    const char* getNotifyName(UInt32 notifyIndex)    const override;
    Float32     getNotifyTime(UInt32 notifyIndex)    const override;
    Float32     getNotifyPayload(UInt32 notifyIndex) const override;

    // ===== Binary serialization =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== Setters =====
    void setName(const std::string& name) { _name = name; }
    void setDuration(Float32 duration) { _duration = duration; }
    void setTicksPerSecond(Float32 tps) { _ticksPerSecond = tps; }
    void setGuid(const ayt::math::FGuid& guid) { _guid = guid; }
    void addTrack(const AnimTrack& track);
    void addNotify(const AnimNotifyMarker& notify) { _notifies.push_back(notify); }

    // ===== Getters =====
    const ayt::math::FGuid& getGuid() const { return _guid; }
    const std::vector<AnimNotifyMarker>& getNotifies() const { return _notifies; }

    // ===== 创建测试数据 =====
    void createTestAnimation();

private:
    void clear();

    std::string _name;
    Float32 _duration = 0.0f;
    Float32 _ticksPerSecond = 30.0f;
    std::vector<AnimTrack> _tracks;
    // Phase 1.5: time-keyed event markers. Stored in arbitrary order on
    // addNotify; loadFromBinary / saveToBinary keep them in insertion order
    // (sorted-by-time invariant is enforced upstream by the converter when
    // the file is authored; AnimationPlayer treats the list as sorted and
    // does an early-exit linear scan, which is correct for unsorted input
    // too because the notify count per clip is tiny).
    std::vector<AnimNotifyMarker> _notifies;
    // Path lives on IResource::_path — do not redeclare (shadowing caused
    // load() to write a different string than getPath() returned).
    ayt::math::FGuid _guid;  // 资源唯一标识
};

} // namespace ayt::resource