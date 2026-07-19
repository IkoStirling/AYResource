#include "assetsImpl/AYSkeleton.h"
#include <aymath/MathTypes.h>
#include "aymath/MathUtils.h"
#include "AYIO.h"
#include <AYLog.h>
#include <cstring>

namespace ayt::resource
{

// ===== 常量 =====
static constexpr UInt32 SKELETON_MAGIC = 0x534B4C4E;  // 'SKLN'
static constexpr UInt16 SKELETON_VERSION = 1;

// ===== Skeleton 文件头 =====
#pragma pack(push, 1)
struct SkeletonBinaryHeader {
    UInt32 magic;
    UInt16 version;
    FGuid guid;                // 资源唯一标识 (16 bytes)
    UInt8  flags;
    UInt8  boneCount;
    UInt32 boneDataSize;
};
#pragma pack(pop)

// ===== Skeleton =====

Skeleton::Skeleton() = default;

void Skeleton::clear() {
    _bones.clear();
    _inverseBindMatrices.clear();
    _localPositions.clear();
    _localRotations.clear();
    _localScales.clear();
    _rootIndices.clear();
    _boneNameMap.clear();
    _path.clear();
}

bool Skeleton::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t Skeleton::sizeInBytes() const {
    size_t size = sizeof(Skeleton);
    for (const auto& bone : _bones) {
        size += bone.name.capacity();
    }
    return size;
}

int Skeleton::findBone(const char* name) const {
    auto it = _boneNameMap.find(name);
    if (it != _boneNameMap.end()) {
        return static_cast<int>(it->second);
    }
    return -1;
}

int Skeleton::getParentBoneIndex(size_t boneIndex) const {
    if (boneIndex >= _bones.size()) {
        return -1;
    }
    return _bones[boneIndex].parentIndex;
}

void Skeleton::setBoneCount(size_t count) {
    _bones.resize(count);
    _inverseBindMatrices.resize(count);
    _localPositions.resize(count);
    _localRotations.resize(count);
    _localScales.resize(count);
}

void Skeleton::setBone(size_t index, const Bone& bone) {
    if (index >= _bones.size()) return;
    _bones[index] = bone;
    _inverseBindMatrices[index] = bone.inverseBindMatrix;
    _localPositions[index] = bone.localPosition;
    _localRotations[index] = bone.localRotation;
    _localScales[index] = bone.localScale;
    _boneNameMap[bone.name] = index;
    if (bone.parentIndex < 0) {
        _rootIndices.push_back(static_cast<int>(index));
    }
}

void Skeleton::addBone(const Bone& bone) {
    size_t index = _bones.size();
    _bones.push_back(bone);
    _inverseBindMatrices.push_back(bone.inverseBindMatrix);
    _localPositions.push_back(bone.localPosition);
    _localRotations.push_back(bone.localRotation);
    _localScales.push_back(bone.localScale);
    _boneNameMap[bone.name] = index;
    if (bone.parentIndex < 0) {
        _rootIndices.push_back(static_cast<int>(index));
    }
}

void Skeleton::createTestSkeleton() {
    clear();

    // 创建简单的手臂骨骼层级
    // Root -> UpperArm -> LowerArm -> Hand
    Bone root;
    root.name = "Root";
    root.parentIndex = -1;
    root.inverseBindMatrix = ayt::math::Float4x4::identity();
    root.localPosition = ayt::math::FVector3(0, 0, 0);
    root.localRotation = ayt::math::FQuaternion(0, 0, 0, 1);
    root.localScale = ayt::math::FVector3(1, 1, 1);
    addBone(root);

    Bone upperArm;
    upperArm.name = "UpperArm";
    upperArm.parentIndex = 0;  // Root
    upperArm.inverseBindMatrix = ayt::math::Float4x4::identity();
    upperArm.localPosition = ayt::math::FVector3(0, 1, 0);
    upperArm.localRotation = ayt::math::FQuaternion(0, 0, 0, 1);
    upperArm.localScale = ayt::math::FVector3(1, 1, 1);
    addBone(upperArm);

    Bone lowerArm;
    lowerArm.name = "LowerArm";
    lowerArm.parentIndex = 1;  // UpperArm
    lowerArm.inverseBindMatrix = ayt::math::Float4x4::identity();
    lowerArm.localPosition = ayt::math::FVector3(0, 1, 0);
    lowerArm.localRotation = ayt::math::FQuaternion(0, 0, 0, 1);
    lowerArm.localScale = ayt::math::FVector3(1, 1, 1);
    addBone(lowerArm);

    Bone hand;
    hand.name = "Hand";
    hand.parentIndex = 2;  // LowerArm
    hand.inverseBindMatrix = ayt::math::Float4x4::identity();
    hand.localPosition = ayt::math::FVector3(0, 1, 0);
    hand.localRotation = ayt::math::FQuaternion(0, 0, 0, 1);
    hand.localScale = ayt::math::FVector3(1, 1, 1);
    addBone(hand);

    _loaded = true;
}

bool Skeleton::load(const std::string& path) {
    _path = path;

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(SkeletonBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Skeleton::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(SkeletonBinaryHeader)) {
        ayt::log::error("[Skeleton] loadFromBinary failed: invalid data or size=%zu", size);
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const SkeletonBinaryHeader* header = reinterpret_cast<const SkeletonBinaryHeader*>(ptr);

    // 验证 magic
    if (header->magic != SKELETON_MAGIC) {
        ayt::log::error("[Skeleton] loadFromBinary failed: magic=0x%08X expected=0x%08X",
                        header->magic, SKELETON_MAGIC);
        return false;
    }

    // 验证版本
    if (header->version != SKELETON_VERSION) {
        ayt::log::error("[Skeleton] loadFromBinary failed: version=%d expected=%d",
                        header->version, SKELETON_VERSION);
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    UInt32 offset = sizeof(SkeletonBinaryHeader);
    UInt32 boneCount = header->boneCount;

    _bones.resize(boneCount);
    _inverseBindMatrices.resize(boneCount);
    _localPositions.resize(boneCount);
    _localRotations.resize(boneCount);
    _localScales.resize(boneCount);
    _boneNameMap.reserve(boneCount);

    for (UInt32 i = 0; i < boneCount; i++) {
        if (offset + sizeof(UInt32) > size) return false;
        UInt32 nameLength = *reinterpret_cast<const UInt32*>(ptr + offset);
        offset += sizeof(UInt32);

        if (offset + nameLength > size) return false;
        _bones[i].name = std::string(reinterpret_cast<const char*>(ptr + offset), nameLength);
        offset += nameLength;

        if (offset + sizeof(Int32) > size) return false;
        _bones[i].parentIndex = *reinterpret_cast<const Int32*>(ptr + offset);
        offset += sizeof(Int32);

        if (offset + sizeof(ayt::math::Float4x4) > size) return false;
        _bones[i].inverseBindMatrix = *reinterpret_cast<const ayt::math::Float4x4*>(ptr + offset);
        offset += sizeof(ayt::math::Float4x4);

        // 新版本: local transforms
        if (offset + sizeof(ayt::math::FVector3) > size) return false;
        _bones[i].localPosition = *reinterpret_cast<const ayt::math::FVector3*>(ptr + offset);
        offset += sizeof(ayt::math::FVector3);

        if (offset + sizeof(ayt::math::FQuaternion) > size) return false;
        _bones[i].localRotation = *reinterpret_cast<const ayt::math::FQuaternion*>(ptr + offset);
        offset += sizeof(ayt::math::FQuaternion);

        if (offset + sizeof(ayt::math::FVector3) > size) return false;
        _bones[i].localScale = *reinterpret_cast<const ayt::math::FVector3*>(ptr + offset);
        offset += sizeof(ayt::math::FVector3);

        _inverseBindMatrices[i] = _bones[i].inverseBindMatrix;
        _localPositions[i] = _bones[i].localPosition;
        _localRotations[i] = _bones[i].localRotation;
        _localScales[i] = _bones[i].localScale;
        _boneNameMap[_bones[i].name] = i;

        if (_bones[i].parentIndex < 0) {
            _rootIndices.push_back(static_cast<int>(i));
        }
    }

    _loaded = true;
    return true;
}

bool Skeleton::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t totalSize = sizeof(SkeletonBinaryHeader);
    for (const auto& bone : _bones) {
        totalSize += sizeof(UInt32);  // name length
        totalSize += bone.name.size();  // name
        totalSize += sizeof(Int32);  // parent index
        totalSize += sizeof(ayt::math::Float4x4);  // inverse bind matrix
        totalSize += sizeof(ayt::math::FVector3);  // local position
        totalSize += sizeof(ayt::math::FQuaternion);  // local rotation
        totalSize += sizeof(ayt::math::FVector3);  // local scale
    }

    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Header
    SkeletonBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = SKELETON_MAGIC;
    header.version = SKELETON_VERSION;
    header.guid = _guid;
    header.flags = 0;
    header.boneCount = static_cast<UInt8>(_bones.size());

    // 计算 bone data size
    size_t boneDataSize = 0;
    for (const auto& bone : _bones) {
        boneDataSize += sizeof(UInt32) + bone.name.size() + sizeof(Int32)
                      + sizeof(ayt::math::Float4x4)
                      + sizeof(ayt::math::FVector3)  // local position
                      + sizeof(ayt::math::FQuaternion)  // local rotation
                      + sizeof(ayt::math::FVector3);  // local scale
    }
    header.boneDataSize = static_cast<UInt32>(boneDataSize);

    std::memcpy(ptr, &header, sizeof(header));
    UInt32 offset = sizeof(SkeletonBinaryHeader);

    // 写入骨骼数据
    for (size_t i = 0; i < _bones.size(); i++) {
        const auto& bone = _bones[i];

        UInt32 nameLength = static_cast<UInt32>(bone.name.size());
        *reinterpret_cast<UInt32*>(ptr + offset) = nameLength;
        offset += sizeof(UInt32);

        std::memcpy(ptr + offset, bone.name.data(), nameLength);
        offset += nameLength;

        *reinterpret_cast<Int32*>(ptr + offset) = bone.parentIndex;
        offset += sizeof(Int32);

        *reinterpret_cast<ayt::math::Float4x4*>(ptr + offset) = bone.inverseBindMatrix;
        offset += sizeof(ayt::math::Float4x4);

        *reinterpret_cast<ayt::math::FVector3*>(ptr + offset) = bone.localPosition;
        offset += sizeof(ayt::math::FVector3);

        *reinterpret_cast<ayt::math::FQuaternion*>(ptr + offset) = bone.localRotation;
        offset += sizeof(ayt::math::FQuaternion);

        *reinterpret_cast<ayt::math::FVector3*>(ptr + offset) = bone.localScale;
        offset += sizeof(ayt::math::FVector3);
    }

    return true;
}

} // namespace ayt::resource