#include "assetsImpl/AYAnimation.h"
#include <AYMathTypes.h>
#include <cstring>
#include <limits>

namespace ayt::resource
{

Animation::Animation() = default;

void Animation::clear() {
    _name.clear();
    _duration = 0.0f;
    _ticksPerSecond = 30.0f;
    _tracks.clear();
}

bool Animation::load(const std::string& path) {
    _path = path;
    // 使用 Loader 加载，此处不做重复实现
    return false;
}

bool Animation::unload() {
    clear();
    _path.clear();
    return true;
}

size_t Animation::sizeInBytes() const {
    size_t size = sizeof(UInt32) * 2; // magic, version
    size += sizeof(UInt32) + _name.size();
    size += sizeof(Float32) * 2; // duration, ticksPerSecond
    size += sizeof(UInt32); // track count
    for (const auto& track : _tracks) {
        size += sizeof(UInt32) + track.nodeName.size();
        size += sizeof(UInt32) + track.property.size();
        size += sizeof(UInt8); // valueType
        size += sizeof(UInt32) + track.times.size() * sizeof(Float32);
        size += sizeof(UInt32) + track.values.size() * sizeof(Float32);
    }
    return size;
}

const char* Animation::getTrackNodeName(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return nullptr;
    return _tracks[trackIndex].nodeName.c_str();
}

const char* Animation::getTrackProperty(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return nullptr;
    return _tracks[trackIndex].property.c_str();
}

AnimTrackType Animation::getTrackType(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return AnimTrackType::Vector3;
    return _tracks[trackIndex].valueType;
}

UInt32 Animation::getTrackKeyframeCount(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return 0;
    return static_cast<UInt32>(_tracks[trackIndex].times.size());
}

const Float32* Animation::getTrackTimes(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return nullptr;
    return _tracks[trackIndex].times.data();
}

const ayt::math::FVector3* Animation::getTrackVector3Values(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return nullptr;
    return reinterpret_cast<const ayt::math::FVector3*>(_tracks[trackIndex].values.data());
}

const ayt::math::FQuaternion* Animation::getTrackQuaternionValues(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return nullptr;
    return reinterpret_cast<const ayt::math::FQuaternion*>(_tracks[trackIndex].values.data());
}

const Float32* Animation::getTrackFloatValues(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return nullptr;
    return _tracks[trackIndex].values.data();
}

const Float32* Animation::getTrackValues(UInt32 trackIndex) const {
    if (trackIndex >= _tracks.size()) return nullptr;
    return _tracks[trackIndex].values.data();
}

void Animation::addTrack(const AnimTrack& track) {
    _tracks.push_back(track);
}

void Animation::createTestAnimation() {
    _name = "TestAnim";
    _duration = 1.0f;
    _ticksPerSecond = 30.0f;

    AnimTrack track;
    track.nodeName = "Bone001";
    track.property = "position";
    track.valueType = AnimTrackType::Vector3;
    track.times = {0.0f, 0.5f, 1.0f};
    track.values = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    _tracks.push_back(track);

    _loaded = true;
}

bool Animation::loadFromBinary(const void* data, size_t size) {
    if (!data || size < 16) return false;

    const UInt8* ptr = static_cast<const UInt8*>(data);
    size_t remaining = size;

    // 读取 header
    UInt32 magic, version;
    memcpy(&magic, ptr, sizeof(UInt32));
    ptr += sizeof(UInt32);
    remaining -= sizeof(UInt32);

    memcpy(&version, ptr, sizeof(UInt32));
    ptr += sizeof(UInt32);
    remaining -= sizeof(UInt32);

    if (magic != IAnimation::MAGIC) return false;
    if (version != IAnimation::VERSION) return false;

    // 读取 name
    UInt32 nameLen;
    memcpy(&nameLen, ptr, sizeof(UInt32));
    ptr += sizeof(UInt32);
    remaining -= sizeof(UInt32);

    if (nameLen > remaining) return false;
    _name = std::string(reinterpret_cast<const char*>(ptr), nameLen);
    ptr += nameLen;
    remaining -= nameLen;

    // 读取 duration 和 ticksPerSecond
    memcpy(&_duration, ptr, sizeof(Float32));
    ptr += sizeof(Float32);
    remaining -= sizeof(Float32);

    memcpy(&_ticksPerSecond, ptr, sizeof(Float32));
    ptr += sizeof(Float32);
    remaining -= sizeof(Float32);

    // 读取 tracks
    UInt32 trackCount;
    memcpy(&trackCount, ptr, sizeof(UInt32));
    ptr += sizeof(UInt32);
    remaining -= sizeof(UInt32);

    _tracks.resize(trackCount);
    for (UInt32 i = 0; i < trackCount; i++) {
        // nodeName
        UInt32 nodeLen;
        memcpy(&nodeLen, ptr, sizeof(UInt32));
        ptr += sizeof(UInt32);
        remaining -= sizeof(UInt32);

        if (nodeLen > remaining) return false;
        _tracks[i].nodeName = std::string(reinterpret_cast<const char*>(ptr), nodeLen);
        ptr += nodeLen;
        remaining -= nodeLen;

        // property
        UInt32 propLen;
        memcpy(&propLen, ptr, sizeof(UInt32));
        ptr += sizeof(UInt32);
        remaining -= sizeof(UInt32);

        if (propLen > remaining) return false;
        _tracks[i].property = std::string(reinterpret_cast<const char*>(ptr), propLen);
        ptr += propLen;
        remaining -= propLen;

        memcpy(&_tracks[i].valueType, ptr, sizeof(UInt8));
        ptr += sizeof(UInt8);
        remaining -= sizeof(UInt8);

        // times
        UInt32 timeCount;
        memcpy(&timeCount, ptr, sizeof(UInt32));
        ptr += sizeof(UInt32);
        remaining -= sizeof(UInt32);

        if (timeCount * sizeof(Float32) > remaining) return false;
        _tracks[i].times.resize(timeCount);
        memcpy(_tracks[i].times.data(), ptr, timeCount * sizeof(Float32));
        ptr += timeCount * sizeof(Float32);
        remaining -= timeCount * sizeof(Float32);

        // values
        UInt32 valueCount;
        memcpy(&valueCount, ptr, sizeof(UInt32));
        ptr += sizeof(UInt32);
        remaining -= sizeof(UInt32);

        if (valueCount * sizeof(Float32) > remaining) return false;
        _tracks[i].values.resize(valueCount);
        memcpy(_tracks[i].values.data(), ptr, valueCount * sizeof(Float32));
        ptr += valueCount * sizeof(Float32);
        remaining -= valueCount * sizeof(Float32);
    }

    _loaded = true;
    return true;
}

bool Animation::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t totalSize = sizeInBytes();
    outData.resize(totalSize);

    UInt8* ptr = outData.data();

    // 写入 header
    UInt32 magic = IAnimation::MAGIC;
    UInt32 version = IAnimation::VERSION;
    memcpy(ptr, &magic, sizeof(UInt32));
    ptr += sizeof(UInt32);

    memcpy(ptr, &version, sizeof(UInt32));
    ptr += sizeof(UInt32);

    // 写入 name
    UInt32 nameLen = static_cast<UInt32>(_name.size());
    memcpy(ptr, &nameLen, sizeof(UInt32));
    ptr += sizeof(UInt32);
    memcpy(ptr, _name.data(), nameLen);
    ptr += nameLen;

    // 写入 duration 和 ticksPerSecond
    memcpy(ptr, &_duration, sizeof(Float32));
    ptr += sizeof(Float32);

    memcpy(ptr, &_ticksPerSecond, sizeof(Float32));
    ptr += sizeof(Float32);

    // 写入 tracks
    UInt32 trackCount = static_cast<UInt32>(_tracks.size());
    memcpy(ptr, &trackCount, sizeof(UInt32));
    ptr += sizeof(UInt32);

    for (const auto& track : _tracks) {
        // nodeName
        UInt32 nodeLen = static_cast<UInt32>(track.nodeName.size());
        memcpy(ptr, &nodeLen, sizeof(UInt32));
        ptr += sizeof(UInt32);
        memcpy(ptr, track.nodeName.data(), nodeLen);
        ptr += nodeLen;

        // property
        UInt32 propLen = static_cast<UInt32>(track.property.size());
        memcpy(ptr, &propLen, sizeof(UInt32));
        ptr += sizeof(UInt32);
        memcpy(ptr, track.property.data(), propLen);
        ptr += propLen;

        // valueType
        memcpy(ptr, &track.valueType, sizeof(UInt8));
        ptr += sizeof(UInt8);

        // times
        UInt32 timeCount = static_cast<UInt32>(track.times.size());
        memcpy(ptr, &timeCount, sizeof(UInt32));
        ptr += sizeof(UInt32);
        memcpy(ptr, track.times.data(), timeCount * sizeof(Float32));
        ptr += timeCount * sizeof(Float32);

        // values
        UInt32 valueCount = static_cast<UInt32>(track.values.size());
        memcpy(ptr, &valueCount, sizeof(UInt32));
        ptr += sizeof(UInt32);
        memcpy(ptr, track.values.data(), valueCount * sizeof(Float32));
        ptr += valueCount * sizeof(Float32);
    }

    return true;
}

} // namespace ayt::resource