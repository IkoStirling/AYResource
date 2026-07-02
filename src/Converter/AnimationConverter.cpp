#include "Converter\AnimationConverter.h"
#include "AYAnimation.h"
#include "AYFile.h"
#include <AYGuid.h>

namespace ayt::resource
{

AnimationConverter::AnimationConverter() = default;

AnimationConverter::AnimationConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void AnimationConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void AnimationConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

ConversionResult AnimationConverter::convert() {
    ConversionResult result;
    //简单 Converter 直接转换，复杂 Converter 由 Parser 驱动
    return result;
}

std::vector<ConversionResult::ConvertedResource> AnimationConverter::convertAll(
    const std::vector<AnimationData>& animations,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (size_t i = 0; i < animations.size(); i++) {
        const auto& animData = animations[i];
        std::string name = animData.name.empty()
            ? baseName + "_" + std::to_string(i) + ".ayanm"
            : baseName + "_" + animData.name + ".ayanm";

        std::string virtualPath = "animations/" + name;

        // 创建 Animation 并填充数据
        Animation animation;
        animation.setName(animData.name);
        animation.setDuration(animData.duration);
        animation.setTicksPerSecond(animData.ticksPerSecond);

        for (const auto& trackData : animData.tracks) {
            AnimTrack track;
            track.nodeName = trackData.targetNode;
            track.property = trackData.property;
            track.times = trackData.times;
            track.values = trackData.values;
            animation.addTrack(track);
        }

        // 构建动画内容数据并计算 GUID（不包含header）
        std::vector<UInt8> animContentData;
        UInt32 nameLen = static_cast<UInt32>(animData.name.size());
        animContentData.resize(sizeof(UInt32) + nameLen + sizeof(Float32) * 2); // name + duration + ticksPerSecond
        UInt8* ptr = animContentData.data();
        *reinterpret_cast<UInt32*>(ptr) = nameLen; ptr += sizeof(UInt32);
        memcpy(ptr, animData.name.data(), nameLen); ptr += nameLen;
        *reinterpret_cast<Float32*>(ptr) = animData.duration; ptr += sizeof(Float32);
        *reinterpret_cast<Float32*>(ptr) = animData.ticksPerSecond;
        // 添加轨道数据
        for (const auto& trackData : animData.tracks) {
            UInt32 nodeLen = static_cast<UInt32>(trackData.targetNode.size());
            UInt32 propLen = static_cast<UInt32>(trackData.property.size());
            animContentData.resize(animContentData.size() + sizeof(UInt32) * 4 + nodeLen + propLen + trackData.times.size() * sizeof(Float32) + trackData.values.size() * sizeof(Float32));
            ptr = animContentData.data() + animContentData.size() - (sizeof(UInt32) * 4 + nodeLen + propLen + trackData.times.size() * sizeof(Float32) + trackData.values.size() * sizeof(Float32));
            *reinterpret_cast<UInt32*>(ptr) = nodeLen; ptr += sizeof(UInt32);
            memcpy(ptr, trackData.targetNode.data(), nodeLen); ptr += nodeLen;
            *reinterpret_cast<UInt32*>(ptr) = propLen; ptr += sizeof(UInt32);
            memcpy(ptr, trackData.property.data(), propLen); ptr += propLen;
            UInt32 timeCount = static_cast<UInt32>(trackData.times.size());
            *reinterpret_cast<UInt32*>(ptr) = timeCount; ptr += sizeof(UInt32);
            memcpy(ptr, trackData.times.data(), timeCount * sizeof(Float32)); ptr += timeCount * sizeof(Float32);
            UInt32 valueCount = static_cast<UInt32>(trackData.values.size());
            *reinterpret_cast<UInt32*>(ptr) = valueCount; ptr += sizeof(UInt32);
            memcpy(ptr, trackData.values.data(), valueCount * sizeof(Float32));
        }
        lastGuid = ayt::storage::Guid::computeFromData(animContentData.data(), animContentData.size());
        animation.setGuid(lastGuid);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        if (!animation.saveToBinary(binaryData)) {
            continue;
        }

        // 写入输出目录
        if (!outputDir.empty()) {
            std::string fullPath = outputDir + "/" + virtualPath;
            if (!ayt::io::File::exists(fullPath)) {
                writeFile(fullPath, binaryData.data(), binaryData.size());
            }
        }

        ConversionResult::ConvertedResource res;
        res.guid = lastGuid;
        res.path = virtualPath;
        res.type = "Animation";
        res.size = static_cast<int64_t>(binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource