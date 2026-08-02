#include "assetsImpl/AYSkeleton.h"
#include <aymath/MathTypes.h>
#include "aymath/MathUtils.h"
#include "AYIO.h"
#include <AYLog.h>
#include <cstring>
#include <vector>

namespace ayt::resource
{

// ===== 常量 =====
static constexpr UInt32 SKELETON_MAGIC = 0x534B4C4E;  // 'SKLN'
// v1: boneCount was UInt8 (truncates at 256) — still readable via boneDataSize scan.
// v2: boneCount is UInt32.
static constexpr UInt16 SKELETON_VERSION_V1 = 1;
static constexpr UInt16 SKELETON_VERSION    = 2;

#pragma pack(push, 1)
// On-disk v1 (legacy). Do not change layout — existing .ayskel files use this.
struct SkeletonBinaryHeaderV1 {
    UInt32 magic;
    UInt16 version;
    FGuid  guid;
    UInt8  flags;
    UInt8  boneCount;      // truncated for skeletons with >255 bones
    UInt32 boneDataSize;
};

// On-disk v2.
struct SkeletonBinaryHeaderV2 {
    UInt32 magic;
    UInt16 version;
    FGuid  guid;
    UInt8  flags;
    UInt8  reserved;       // was v1 boneCount slot; unused
    UInt32 boneCount;
    UInt32 boneDataSize;
};
#pragma pack(pop)

static_assert(sizeof(SkeletonBinaryHeaderV1) == 28, "v1 .ayskel header size");
static_assert(sizeof(SkeletonBinaryHeaderV2) == 32, "v2 .ayskel header size");

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
    Bone root;
    root.name = "Root";
    root.parentIndex = -1;
    root.localPosition = ayt::math::FVector3(0, 0, 0);
    root.localRotation = ayt::math::FQuaternion::identity();
    root.localScale = ayt::math::FVector3(1, 1, 1);
    root.inverseBindMatrix = ayt::math::Float4x4::identity();
    addBone(root);
}

bool Skeleton::load(const std::string& path) {
    _path = path;
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        ayt::log::error("[Skeleton] load failed: cannot open '%s'", path.c_str());
        return false;
    }
    const size_t size = static_cast<size_t>(file.size());
    std::vector<UInt8> data(size);
    if (file.read(data.data(), size) != size) {
        ayt::log::error("[Skeleton] load failed: short read '%s'", path.c_str());
        return false;
    }
    return loadFromBinary(data.data(), data.size());
}

namespace {

// Read one bone record at *offset. Uses sizeof(FVector3/FQuaternion/Float4x4)
// to match Skeleton::saveToBinary (SSE-padded math types).
bool readBoneRecord(const UInt8* ptr, size_t size, UInt32& offset, Bone& out)
{
    if (offset + sizeof(UInt32) > size) return false;
    const UInt32 nameLength = *reinterpret_cast<const UInt32*>(ptr + offset);
    offset += sizeof(UInt32);

    if (nameLength > 512 || offset + nameLength > size) return false;
    out.name.assign(reinterpret_cast<const char*>(ptr + offset), nameLength);
    offset += nameLength;

    if (offset + sizeof(Int32) > size) return false;
    std::memcpy(&out.parentIndex, ptr + offset, sizeof(Int32));
    offset += sizeof(Int32);

    // memcpy — do NOT load SSE math types via unaligned pointer deref
    // (AYMATH AVX2 builds may use aligned loads → AV on odd offsets).
    if (offset + sizeof(ayt::math::Float4x4) > size) return false;
    std::memcpy(&out.inverseBindMatrix, ptr + offset, sizeof(ayt::math::Float4x4));
    offset += sizeof(ayt::math::Float4x4);

    if (offset + sizeof(ayt::math::FVector3) > size) return false;
    std::memcpy(&out.localPosition, ptr + offset, sizeof(ayt::math::FVector3));
    offset += sizeof(ayt::math::FVector3);

    if (offset + sizeof(ayt::math::FQuaternion) > size) return false;
    std::memcpy(&out.localRotation, ptr + offset, sizeof(ayt::math::FQuaternion));
    offset += sizeof(ayt::math::FQuaternion);

    if (offset + sizeof(ayt::math::FVector3) > size) return false;
    std::memcpy(&out.localScale, ptr + offset, sizeof(ayt::math::FVector3));
    offset += sizeof(ayt::math::FVector3);

    return true;
}

size_t boneRecordBytes(const Bone& bone)
{
    return sizeof(UInt32) + bone.name.size() + sizeof(Int32)
         + sizeof(ayt::math::Float4x4)
         + sizeof(ayt::math::FVector3)
         + sizeof(ayt::math::FQuaternion)
         + sizeof(ayt::math::FVector3);
}

} // namespace

bool Skeleton::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(SkeletonBinaryHeaderV1)) {
        ayt::log::error("[Skeleton] loadFromBinary failed: invalid data or size=%zu", size);
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const UInt32 magic = *reinterpret_cast<const UInt32*>(ptr);
    const UInt16 version = *reinterpret_cast<const UInt16*>(ptr + 4);

    if (magic != SKELETON_MAGIC) {
        ayt::log::error("[Skeleton] loadFromBinary failed: magic=0x%08X expected=0x%08X",
                        magic, SKELETON_MAGIC);
        return false;
    }

    UInt32 offset = 0;
    UInt32 dataEnd = 0;
    UInt32 headerBoneCount = 0;

    if (version == SKELETON_VERSION_V1) {
        if (size < sizeof(SkeletonBinaryHeaderV1)) return false;
        const auto* header = reinterpret_cast<const SkeletonBinaryHeaderV1*>(ptr);
        _guid = header->guid;
        headerBoneCount = header->boneCount;
        offset = sizeof(SkeletonBinaryHeaderV1);
        dataEnd = offset + header->boneDataSize;
        if (dataEnd > size) {
            ayt::log::error("[Skeleton] loadFromBinary v1: boneDataSize overflows file");
            return false;
        }
        // v1 boneCount is UInt8 and silently truncates (>255 → low byte).
        // Always scan the full bone payload; do not trust headerBoneCount.
        std::vector<Bone> scanned;
        UInt32 scan = offset;
        while (scan < dataEnd) {
            Bone bone;
            const UInt32 before = scan;
            if (!readBoneRecord(ptr, dataEnd, scan, bone)) {
                ayt::log::error("[Skeleton] loadFromBinary v1: corrupt bone at offset %u "
                                "(headerCount=%u scanned=%zu)",
                                before, headerBoneCount, scanned.size());
                return false;
            }
            scanned.push_back(std::move(bone));
        }
        if (headerBoneCount != scanned.size()) {
            ayt::log::warn("[Skeleton] loadFromBinary v1: header boneCount=%u truncated; "
                           "recovered %zu bones from boneDataSize",
                           headerBoneCount, scanned.size());
        }
        setBoneCount(scanned.size());
        for (size_t i = 0; i < scanned.size(); ++i) {
            setBone(i, scanned[i]);
        }
        _loaded = true;
        return true;
    }

    if (version == SKELETON_VERSION) {
        if (size < sizeof(SkeletonBinaryHeaderV2)) return false;
        const auto* header = reinterpret_cast<const SkeletonBinaryHeaderV2*>(ptr);
        _guid = header->guid;
        headerBoneCount = header->boneCount;
        offset = sizeof(SkeletonBinaryHeaderV2);
        dataEnd = offset + header->boneDataSize;
        if (dataEnd > size) {
            ayt::log::error("[Skeleton] loadFromBinary v2: boneDataSize overflows file");
            return false;
        }

        setBoneCount(headerBoneCount);
        for (UInt32 i = 0; i < headerBoneCount; ++i) {
            Bone bone;
            if (!readBoneRecord(ptr, dataEnd, offset, bone)) {
                ayt::log::error("[Skeleton] loadFromBinary v2: corrupt bone %u/%u",
                                i, headerBoneCount);
                clear();
                return false;
            }
            // Clamp illegal parent refs (defensive — bad FBX / truncated v1 upgrades).
            if (bone.parentIndex >= 0
                && static_cast<UInt32>(bone.parentIndex) >= headerBoneCount) {
                ayt::log::warn("[Skeleton] bone %u '%s' parentIndex=%d OOB; treating as root",
                               i, bone.name.c_str(), bone.parentIndex);
                bone.parentIndex = -1;
            }
            setBone(i, bone);
        }
        _loaded = true;
        return true;
    }

    ayt::log::error("[Skeleton] loadFromBinary failed: version=%d (supported 1..%d)",
                    version, SKELETON_VERSION);
    return false;
}

bool Skeleton::saveToBinary(std::vector<UInt8>& outData) const {
    size_t boneDataSize = 0;
    for (const auto& bone : _bones) {
        boneDataSize += boneRecordBytes(bone);
    }

    const size_t totalSize = sizeof(SkeletonBinaryHeaderV2) + boneDataSize;
    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    SkeletonBinaryHeaderV2 header;
    std::memset(&header, 0, sizeof(header));
    header.magic = SKELETON_MAGIC;
    header.version = SKELETON_VERSION;
    header.guid = _guid;
    header.flags = 0;
    header.reserved = 0;
    header.boneCount = static_cast<UInt32>(_bones.size());
    header.boneDataSize = static_cast<UInt32>(boneDataSize);

    std::memcpy(ptr, &header, sizeof(header));
    UInt32 offset = sizeof(SkeletonBinaryHeaderV2);

    for (size_t i = 0; i < _bones.size(); i++) {
        const auto& bone = _bones[i];

        const         UInt32 nameLength = static_cast<UInt32>(bone.name.size());
        std::memcpy(ptr + offset, &nameLength, sizeof(UInt32));
        offset += sizeof(UInt32);

        std::memcpy(ptr + offset, bone.name.data(), nameLength);
        offset += nameLength;

        std::memcpy(ptr + offset, &bone.parentIndex, sizeof(Int32));
        offset += sizeof(Int32);

        std::memcpy(ptr + offset, &bone.inverseBindMatrix, sizeof(ayt::math::Float4x4));
        offset += sizeof(ayt::math::Float4x4);

        std::memcpy(ptr + offset, &bone.localPosition, sizeof(ayt::math::FVector3));
        offset += sizeof(ayt::math::FVector3);

        std::memcpy(ptr + offset, &bone.localRotation, sizeof(ayt::math::FQuaternion));
        offset += sizeof(ayt::math::FQuaternion);

        std::memcpy(ptr + offset, &bone.localScale, sizeof(ayt::math::FVector3));
        offset += sizeof(ayt::math::FVector3);
    }

    return true;
}

} // namespace ayt::resource
