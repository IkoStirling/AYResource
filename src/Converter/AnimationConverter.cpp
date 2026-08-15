#include "AYResource/Converter/AnimationConverter.h"
#include "AYResource/assetsImpl/Animation.h"
#include "AYIO/File.h"
#include <AYStorage/Guid.h>
#include <unordered_set>

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

// R-02: take 名字清洗 — 与引擎其他资产的 <base>_<name>.<ext> 约定一致
// 规则:
//   1. strip 保留字符 '/\:*?"<>|'
//   2. strip 前导 '.'
//   3. 折叠空白为 '_'
//   4. 空名 fallback 到 take_<index>
//   5. 冲突重命名为 _dup2/_dup3/...
static std::string sanitizeTakeName(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
            || c == '"' || c == '<' || c == '>' || c == '|') {
            s.push_back('_');
        } else if (c == ' ' || c == '\t') {
            s.push_back('_');
        } else {
            s.push_back(c);
        }
    }
    // strip leading dots
    size_t start = 0;
    while (start < s.size() && s[start] == '.') ++start;
    if (start > 0) s.erase(0, start);
    return s;
}

static std::string makeUniquePath(const std::string& baseName,
                                  const std::string& sanitizedTake,
                                  size_t takeIndex,
                                  std::unordered_set<std::string>& used) {
    std::string take = sanitizedTake.empty()
        ? ("take_" + std::to_string(takeIndex))
        : sanitizedTake;
    std::string candidate = baseName + "_" + take + ".ayanm";
    std::string vp = "animations/" + candidate;
    if (used.insert(vp).second) return vp;
    // collision: _dup2 / _dup3 / ...
    for (int n = 2; ; ++n) {
        candidate = baseName + "_" + take + "_dup" + std::to_string(n) + ".ayanm";
        vp = "animations/" + candidate;
        if (used.insert(vp).second) return vp;
    }
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
    std::unordered_set<std::string> usedPaths;

    for (size_t i = 0; i < animations.size(); i++) {
        const auto& animData = animations[i];

        // R-02: 文件名 sanitize + dedupe (取消原来 <base>_<animName> 的硬编码)
        std::string sanitized = sanitizeTakeName(animData.name);
        std::string virtualPath = makeUniquePath(baseName, sanitized, i, usedPaths);
        std::string fileStem = virtualPath.substr(std::string("animations/").size());
        fileStem = fileStem.substr(0, fileStem.find_last_of('.'));

        // 创建 Animation 并填充数据
        Animation animation;
        animation.setName(animData.name);
        animation.setDuration(animData.duration);
        animation.setTicksPerSecond(animData.ticksPerSecond);

        for (const auto& trackData : animData.tracks) {
            AnimTrack track;
            track.nodeName = trackData.targetNode;
            track.property = trackData.property;
            // R-02: 透传 valueType (Phase 0 之前始终默认 Vector3,导致 rotation 被当 Vector3 解释)
            track.valueType = trackData.valueType;
            // Phase 1.2 (P1.2): pipe the per-track blend mode too. Default
            // is Override; intermediate assets authored as additive mark this
            // explicitly so the writer emits the v3 byte.
            track.blendMode = trackData.blendMode;
            track.times = trackData.times;
            track.values = trackData.values;
            animation.addTrack(track);
        }

        // Phase 1.5: also copy any notify markers into the concrete Animation.
        // The intermediate AnimationData's `notifies` is normally empty for
        // FBX-derived assets (FBX has no first-class notify channel); any
        // non-empty list comes from a sidecar or an in-engine author tool.
        for (const auto& notifyData : animData.notifies) {
            AnimNotifyMarker n;
            n.name    = notifyData.name;
            n.time    = notifyData.time;
            n.payload = notifyData.payload;
            animation.addNotify(n);
        }

        // 构建动画内容数据并计算 GUID（不包含header）
        // R-02: 把每条 track 的 valueType 也加进 GUID hash,确保类型变化能改 hash
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
            const size_t perTrack = sizeof(UInt32) * 4 + nodeLen + propLen
                                   + sizeof(UInt8) // valueType
                                   + sizeof(UInt8) // blendMode (Phase 1.2)
                                   + trackData.times.size() * sizeof(Float32)
                                   + trackData.values.size() * sizeof(Float32);
            animContentData.resize(animContentData.size() + perTrack);
            ptr = animContentData.data() + animContentData.size() - perTrack;
            *reinterpret_cast<UInt32*>(ptr) = nodeLen; ptr += sizeof(UInt32);
            memcpy(ptr, trackData.targetNode.data(), nodeLen); ptr += nodeLen;
            *reinterpret_cast<UInt32*>(ptr) = propLen; ptr += sizeof(UInt32);
            memcpy(ptr, trackData.property.data(), propLen); ptr += propLen;
            // R-02: valueType 入 hash
            *reinterpret_cast<UInt8*>(ptr) = static_cast<UInt8>(trackData.valueType);
            ptr += sizeof(UInt8);
            // Phase 1.2 (P1.2): blendMode byte also hashed so flipping
            // a track's blend flag forces a re-import (mirrors saveToBinary).
            *reinterpret_cast<UInt8*>(ptr) = static_cast<UInt8>(trackData.blendMode);
            ptr += sizeof(UInt8);
            UInt32 timeCount = static_cast<UInt32>(trackData.times.size());
            *reinterpret_cast<UInt32*>(ptr) = timeCount; ptr += sizeof(UInt32);
            memcpy(ptr, trackData.times.data(), timeCount * sizeof(Float32)); ptr += timeCount * sizeof(Float32);
            UInt32 valueCount = static_cast<UInt32>(trackData.values.size());
            *reinterpret_cast<UInt32*>(ptr) = valueCount; ptr += sizeof(UInt32);
            memcpy(ptr, trackData.values.data(), valueCount * sizeof(Float32));
        }
        // Phase 1.5: include notify marker bytes in the GUID hash so
        // re-export with changed notify lists bumps the GUID and forces a
        // re-import by ResourceManager. Mirrors saveToBinary()'s notify
        // block layout so the hash stays consistent on disk.
        {
            UInt32 notifyCount = static_cast<UInt32>(animData.notifies.size());
            animContentData.resize(animContentData.size() + sizeof(UInt32));
            memcpy(animContentData.data() + animContentData.size() - sizeof(UInt32),
                   &notifyCount, sizeof(UInt32));
            for (const auto& n : animData.notifies) {
                UInt32 nameLen = static_cast<UInt32>(n.name.size());
                const size_t perNotify = sizeof(UInt32) + nameLen + sizeof(Float32) * 2;
                animContentData.resize(animContentData.size() + perNotify);
                ptr = animContentData.data() + animContentData.size() - perNotify;
                *reinterpret_cast<UInt32*>(ptr) = nameLen; ptr += sizeof(UInt32);
                memcpy(ptr, n.name.data(), nameLen); ptr += nameLen;
                *reinterpret_cast<Float32*>(ptr) = n.time; ptr += sizeof(Float32);
                *reinterpret_cast<Float32*>(ptr) = n.payload;
            }
        }
        lastGuid = ayt::storage::Guid::computeFromData(animContentData.data(), animContentData.size());
        animation.setGuid(lastGuid);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        if (!animation.saveToBinary(binaryData)) {
            continue;
        }

        // Always overwrite. Skipping when the file exists left tests (and
        // re-cooks) reading stale/corrupt .ayanm from prior runs — that
        // manifested as heap corruption when the loaded Animation was
        // destroyed (shared_ptr delete in ValueTypeRoundTrip_*).
        if (!outputDir.empty()) {
            std::string fullPath = outputDir + "/" + virtualPath;
            writeFile(fullPath, binaryData.data(), binaryData.size());
        }

        ConversionResult::ConvertedResource res;
        res.guid = lastGuid;
        res.path = virtualPath;
        res.type = "Animation";
        res.size = static_cast<uint64_t>(binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource