#include "Converter\SkeletonConverter.h"
#include "assetsImpl/AYSkeleton.h"
#include "IAYSkeleton.h"
#include "AYFile.h"
#include <AYGuid.h>
#include <vector>

namespace ayt::resource
{

SkeletonConverter::SkeletonConverter() = default;

SkeletonConverter::SkeletonConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void SkeletonConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void SkeletonConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

ConversionResult SkeletonConverter::convert() {
    ConversionResult result;
    if (!isValid()) {
        return result;
    }
    return result;
}

static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

std::vector<ConversionResult::ConvertedResource> SkeletonConverter::convertAll(
    const std::vector<SkeletonData>& skeletons,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (size_t i = 0; i < skeletons.size(); i++) {
        const auto& skelData = skeletons[i];

        // 创建 Skeleton 并填充数据
        Skeleton skeleton;
        for (const auto& boneData : skelData.bones) {
            Bone bone;
            bone.name = boneData.name;
            bone.parentIndex = boneData.parentIndex;
            bone.inverseBindMatrix = boneData.inverseBindMatrix;
            // R-02: 透传本地 rest pose 字段
            bone.localPosition = boneData.localPosition;
            bone.localRotation = boneData.localRotation;
            bone.localScale = boneData.localScale;
            skeleton.addBone(bone);
        }

        // 计算 GUID（基于骨骼内容数据 — 现在包含 local pose 三元组）
        std::vector<UInt8> boneContentData;
        for (const auto& bd : skelData.bones) {
            UInt32 nameLen = static_cast<UInt32>(bd.name.size());
            const size_t perBone = sizeof(UInt32) + nameLen + sizeof(Int32)
                                 + sizeof(ayt::math::Float4x4)
                                 + sizeof(Float32) * 3   // localPosition
                                 + sizeof(Float32) * 4   // localRotation
                                 + sizeof(Float32) * 3;  // localScale
            boneContentData.resize(boneContentData.size() + perBone);
            UInt8* ptr = boneContentData.data() + boneContentData.size() - perBone;
            *reinterpret_cast<UInt32*>(ptr) = nameLen; ptr += sizeof(UInt32);
            memcpy(ptr, bd.name.data(), nameLen); ptr += nameLen;
            *reinterpret_cast<Int32*>(ptr) = bd.parentIndex; ptr += sizeof(Int32);
            *reinterpret_cast<ayt::math::Float4x4*>(ptr) = bd.inverseBindMatrix;
            ptr += sizeof(ayt::math::Float4x4);
            *reinterpret_cast<Float32*>(ptr +  0) = bd.localPosition.x;
            *reinterpret_cast<Float32*>(ptr +  4) = bd.localPosition.y;
            *reinterpret_cast<Float32*>(ptr +  8) = bd.localPosition.z;
            ptr += sizeof(Float32) * 3;
            *reinterpret_cast<Float32*>(ptr +  0) = bd.localRotation.x;
            *reinterpret_cast<Float32*>(ptr +  4) = bd.localRotation.y;
            *reinterpret_cast<Float32*>(ptr +  8) = bd.localRotation.z;
            *reinterpret_cast<Float32*>(ptr + 12) = bd.localRotation.w;
            ptr += sizeof(Float32) * 4;
            *reinterpret_cast<Float32*>(ptr +  0) = bd.localScale.x;
            *reinterpret_cast<Float32*>(ptr +  4) = bd.localScale.y;
            *reinterpret_cast<Float32*>(ptr +  8) = bd.localScale.z;
        }
        lastGuid = ayt::storage::Guid::computeFromData(boneContentData.data(), boneContentData.size());
        skeleton.setGuid(lastGuid);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        if (!skeleton.saveToBinary(binaryData)) {
            continue;
        }

        // 生成虚拟路径: skeletons/{baseName}_{name}.ayskel
        std::string skelName = skelData.name.empty() ? "Skeleton" : skelData.name;
        std::string outputName = baseName + "_" + skelName + ".ayskel";
        std::string virtualPath = "skeletons/" + outputName;

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
        res.type = "Skeleton";
        res.size = static_cast<int64_t>(binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource
