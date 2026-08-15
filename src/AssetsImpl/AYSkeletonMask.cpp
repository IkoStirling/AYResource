// AYSkeletonMask.cpp — P3.x 刀 1 (2026-08-06) concrete SkeletonMask impl.

#include "AYResource/assetsImpl/SkeletonMask.h"

#include <AYIO.h>
#include <AYLog.h>
#include <AYMath/MathTypes.h>
#include <AYMath/MathUtils.h>

#include <cstring>
#include <vector>

namespace ayt::resource
{

// ===== .aymask v1 binary format =====
//
// MAGIC = 'MASK' = 0x4D41534B  (single fourcc, big-endian-friendly)
//
// Header (36 bytes, packed):
//   UInt32 magic
//   UInt16 version
//   FGuid  guid              (16 bytes — same shape as Skeleton / Animation)
//   UInt8  flags             (reserved, 0)
//   UInt8  hasWildcard       (0 / 1)
//   Float32 wildcardWeight   (0..1, valid only when hasWildcard=1)
//   UInt32 entryCount        (number of named entries on disk)
//   UInt32 entryDataSize     (bytes for the entry payload)
//
// Per entry (variable-length):
//   UInt32 nameLength
//   char   name[nameLength]  (NOT null-terminated on disk)
//   Float32 weight
//
// Layout invariants:
//   - Forward-compat tripwire: loadFromBinary rejects version > VERSION
//   - Bad magic → load fails (defensive)
//   - entryDataSize is authoritative — total file size MUST equal
//     header.size + entryDataSize.

static constexpr ayt::math::UInt32 SKELETON_MASK_MAGIC = 0x4D41534Bu;  // 'MASK'
static constexpr ayt::math::UInt16 SKELETON_MASK_VERSION = 1;

#pragma pack(push, 1)
struct SkeletonMaskBinaryHeaderV1 {
    ayt::math::UInt32 magic;
    ayt::math::UInt16 version;
    ayt::math::FGuid  guid;
    ayt::math::UInt8  flags;
    ayt::math::UInt8  hasWildcard;
    float             wildcardWeight;
    ayt::math::UInt32 entryCount;
    ayt::math::UInt32 entryDataSize;
};
#pragma pack(pop)

static_assert(sizeof(SkeletonMaskBinaryHeaderV1) == 36,
              "v1 .aymask header size mismatch");

// ===== SkeletonMask =====

SkeletonMask::SkeletonMask() {
    // Default type so diagnostics + future loaders see a sensible value.
    // IResource::_type / _loaded / _path default-init in IResource().
}

void SkeletonMask::clear() {
    _guid = ayt::math::FGuid{};
    _entries.clear();
    _hasWildcard = false;
    _wildcardWeight = 0.0f;
    _debugName = "SkeletonMask";
}

bool SkeletonMask::unload() {
    clear();
    _loaded = false;
    return true;
}

std::size_t SkeletonMask::sizeInBytes() const {
    std::size_t s = sizeof(SkeletonMask);
    s += _entries.capacity() * sizeof(SkeletonMaskBone);
    for (const auto& e : _entries) {
        s += e.name.capacity();
    }
    s += _debugName.capacity();
    return s;
}

// === ISkeletonMask overrides ===

std::size_t SkeletonMask::getEntryCount() const {
    return _entries.size() + (_hasWildcard ? 1u : 0u);
}

const SkeletonMaskBone* SkeletonMask::getEntries() const {
    return _entries.data();
}

std::size_t SkeletonMask::getAuthoredBoneCount() const {
    return _entries.size();
}

bool  SkeletonMask::hasWildcard()    const { return _hasWildcard; }
float SkeletonMask::wildcardWeight() const { return _wildcardWeight; }

const char* SkeletonMask::getDebugName() const {
    return _debugName.c_str();
}

// === Authoring API ===

void SkeletonMask::addEntry(const char* boneName, float weight) {
    if (boneName == nullptr) return;
    const float w = (weight < 0.0f) ? 0.0f
                 : (weight > 1.0f) ? 1.0f
                 :                    weight;
    const std::size_t nameLen = std::strlen(boneName);
    if (nameLen == 0) {
        _hasWildcard = true;
        _wildcardWeight = w;
        return;
    }
    for (auto& e : _entries) {
        if (e.name == boneName) {
            e.weight = w;
            return;
        }
    }
    SkeletonMaskBone b{};
    b.name = std::string(boneName, nameLen);
    b.weight = w;
    b.resolvedIndex = -1;
    b.isWildcard = false;
    _entries.push_back(std::move(b));
}

void SkeletonMask::setDebugName(const char* n) {
    _debugName = (n != nullptr) ? n : "";
}

std::shared_ptr<SkeletonMask> SkeletonMask::create() {
    auto m = std::shared_ptr<SkeletonMask>(new SkeletonMask());
    m->_loaded = true;     // in-memory fixture is always "loaded"
    m->_type   = TYPE;
    return m;
}

void SkeletonMask::createTestMask() {
    clear();
    _loaded = true;
    _type   = TYPE;
    // Deterministic GUID for snapshot reproducibility.
    ayt::math::FGuid testGuid;
    testGuid.fromString("00000000-0000-0000-0000-000000000001");
    _guid = testGuid;
    addEntry("Root",     1.0f);
    addEntry("Spine",    0.5f);
    addEntry("UpperArm", 0.0f);   // suppress UpperArm
    addEntry("",         0.25f);  // wildcard — unnamed bones get 0.25
}

// === Binary I/O ===

bool SkeletonMask::load(const std::string& path) {
    _path = path;
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        ayt::log::error("[SkeletonMask] load failed: cannot open '%s'", path.c_str());
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(file.size());
    std::vector<ayt::math::UInt8> data(size);
    if (file.read(data.data(), size) != size) {
        ayt::log::error("[SkeletonMask] load failed: short read '%s'", path.c_str());
        return false;
    }
    return loadFromBinary(data.data(), data.size());
}

namespace {

// Read one named entry at *offset. Returns false if the buffer is
// exhausted before the entry is complete.
bool readMaskEntry(const ayt::math::UInt8* ptr,
                   std::size_t size,
                   std::size_t& offset,
                   SkeletonMaskBone& out)
{
    if (offset + sizeof(ayt::math::UInt32) > size) return false;
    ayt::math::UInt32 nameLength;
    std::memcpy(&nameLength, ptr + offset, sizeof(ayt::math::UInt32));
    offset += sizeof(ayt::math::UInt32);

    if (nameLength > 512 || offset + nameLength > size) return false;
    out.name.assign(reinterpret_cast<const char*>(ptr + offset), nameLength);
    offset += nameLength;

    if (offset + sizeof(float) > size) return false;
    std::memcpy(&out.weight, ptr + offset, sizeof(float));
    offset += sizeof(float);

    out.resolvedIndex = -1;
    out.isWildcard    = false;
    return true;
}

std::size_t maskEntryBytes(const SkeletonMaskBone& e)
{
    return sizeof(ayt::math::UInt32) + e.name.size() + sizeof(float);
}

} // namespace

bool SkeletonMask::loadFromBinary(const void* data, std::size_t size) {
    if (!data || size < sizeof(SkeletonMaskBinaryHeaderV1)) {
        ayt::log::error("[SkeletonMask] loadFromBinary failed: invalid data or size=%zu", size);
        return false;
    }

    clear();
    _type = TYPE;

    const ayt::math::UInt8* ptr = static_cast<const ayt::math::UInt8*>(data);

    ayt::math::UInt32 magic;
    std::memcpy(&magic, ptr, sizeof(magic));
    if (magic != SKELETON_MASK_MAGIC) {
        ayt::log::error("[SkeletonMask] loadFromBinary failed: magic=0x%08X expected=0x%08X",
                        magic, SKELETON_MASK_MAGIC);
        return false;
    }

    ayt::math::UInt16 version;
    std::memcpy(&version, ptr + sizeof(ayt::math::UInt32), sizeof(version));
    if (version != SKELETON_MASK_VERSION) {
        ayt::log::error("[SkeletonMask] loadFromBinary failed: version=%u (supported %u)",
                        version, SKELETON_MASK_VERSION);
        return false;
    }

    const auto* header = reinterpret_cast<const SkeletonMaskBinaryHeaderV1*>(ptr);
    _guid            = header->guid;
    _hasWildcard     = (header->hasWildcard != 0);
    _wildcardWeight  = header->wildcardWeight;

    const std::size_t headerEnd  = sizeof(SkeletonMaskBinaryHeaderV1);
    const std::size_t payloadEnd = headerEnd + header->entryDataSize;
    if (payloadEnd > size) {
        ayt::log::error("[SkeletonMask] loadFromBinary failed: entryDataSize overflows file");
        return false;
    }

    std::size_t offset = headerEnd;
    for (ayt::math::UInt32 i = 0; i < header->entryCount; ++i) {
        SkeletonMaskBone bone;
        const std::size_t before = offset;
        if (!readMaskEntry(ptr, payloadEnd, offset, bone)) {
            ayt::log::error("[SkeletonMask] loadFromBinary failed: corrupt entry %u/%u at offset %zu",
                            i, header->entryCount, before);
            clear();
            return false;
        }
        // Empty name on disk = wildcard (defensive; hasWildcard header bit
        // is the canonical signal but the entry content is also validated).
        if (bone.name.empty()) {
            _hasWildcard    = true;
            _wildcardWeight = bone.weight;
            continue;
        }
        _entries.push_back(std::move(bone));
    }

    _loaded = true;
    return true;
}

bool SkeletonMask::saveToBinary(std::vector<ayt::math::UInt8>& outData) const {
    std::size_t entryDataSize = 0;
    for (const auto& e : _entries) {
        entryDataSize += maskEntryBytes(e);
    }

    const std::size_t totalSize = sizeof(SkeletonMaskBinaryHeaderV1) + entryDataSize;
    outData.resize(totalSize);
    ayt::math::UInt8* ptr = outData.data();

    SkeletonMaskBinaryHeaderV1 header{};
    header.magic          = SKELETON_MASK_MAGIC;
    header.version        = SKELETON_MASK_VERSION;
    header.guid           = _guid;
    header.flags          = 0;
    header.hasWildcard    = _hasWildcard ? ayt::math::UInt8{1} : ayt::math::UInt8{0};
    header.wildcardWeight = _wildcardWeight;
    header.entryCount     = static_cast<ayt::math::UInt32>(_entries.size());
    header.entryDataSize  = static_cast<ayt::math::UInt32>(entryDataSize);

    std::memcpy(ptr, &header, sizeof(header));
    std::size_t offset = sizeof(SkeletonMaskBinaryHeaderV1);

    for (const auto& bone : _entries) {
        const ayt::math::UInt32 nameLength =
            static_cast<ayt::math::UInt32>(bone.name.size());
        std::memcpy(ptr + offset, &nameLength, sizeof(ayt::math::UInt32));
        offset += sizeof(ayt::math::UInt32);
        std::memcpy(ptr + offset, bone.name.data(), nameLength);
        offset += nameLength;
        std::memcpy(ptr + offset, &bone.weight, sizeof(float));
        offset += sizeof(float);
    }
    return true;
}

} // namespace ayt::resource