#include "AYResource/MeshMorphContract.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace ayt::resource
{

namespace {

constexpr UInt32 kMeshMorphSupportedFlags = 0;
constexpr size_t kLegacyTargetFixedBytes = sizeof(UInt32) * 3u + sizeof(Float32);

bool readLEUInt32(const UInt8*& ptr, const UInt8* end, UInt32& out)
{
    if (ptr > end || static_cast<size_t>(end - ptr) < sizeof(UInt32)) {
        return false;
    }
    std::memcpy(&out, ptr, sizeof(UInt32));
    ptr += sizeof(UInt32);
    return true;
}

bool readLEFloat32(const UInt8*& ptr, const UInt8* end, Float32& out)
{
    if (ptr > end || static_cast<size_t>(end - ptr) < sizeof(Float32)) {
        return false;
    }
    std::memcpy(&out, ptr, sizeof(Float32));
    ptr += sizeof(Float32);
    return std::isfinite(out);
}

bool readString(const UInt8*& ptr, const UInt8* end, UInt32 length, std::string& out)
{
    if (ptr > end || static_cast<size_t>(end - ptr) < length) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(ptr), length);
    ptr += length;
    return true;
}

size_t deltaByteSize(UInt8 payloadChannels)
{
    size_t bytes = sizeof(UInt32);
    if ((payloadChannels & kMeshMorphPayloadPosition) != 0u) bytes += 3u * sizeof(Float32);
    if ((payloadChannels & kMeshMorphPayloadNormal) != 0u) bytes += 3u * sizeof(Float32);
    if ((payloadChannels & kMeshMorphPayloadTangent) != 0u) bytes += 4u * sizeof(Float32);
    return bytes;
}

} // namespace

bool readMeshMorphContract(const IMesh& mesh, MeshMorphContract& out, std::string* error)
{
    out = MeshMorphContract{};
    if (error != nullptr) error->clear();

    const IMesh::Extension* ext = mesh.findExtension(kMeshMorphChunkType);
    if (ext == nullptr || ext->size == 0 || ext->data == nullptr) {
        return true;
    }

    const UInt8* ptr = ext->data;
    const UInt8* end = ext->data + ext->size;
    MeshMorphContract parsed;
    auto fail = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };

    UInt32 firstWord = 0;
    if (!readLEUInt32(ptr, end, firstWord)) {
        return fail("MORP chunk truncated at header.");
    }

    UInt32 targetCount = 0;
    if (firstWord == kMeshMorphPayloadMagic) {
        if (!readLEUInt32(ptr, end, parsed.version)
            || !readLEUInt32(ptr, end, parsed.flags)
            || !readLEUInt32(ptr, end, targetCount)) {
            return fail("MORP v2 chunk truncated at header.");
        }
        if (parsed.version != kMeshMorphCurrentVersion) {
            return fail("MORP chunk uses an unsupported version.");
        }
        if ((parsed.flags & ~kMeshMorphSupportedFlags) != 0u) {
            return fail("MORP chunk uses unsupported flags.");
        }
    } else {
        parsed.version = 1;
        parsed.flags = 0;
        targetCount = firstWord;
    }

    const size_t remainingAfterHeader = static_cast<size_t>(end - ptr);
    if (targetCount > remainingAfterHeader / kLegacyTargetFixedBytes) {
        return fail("MORP targetCount exceeds the payload size.");
    }
    parsed.targets.reserve(targetCount);

    for (UInt32 targetIdx = 0; targetIdx < targetCount; ++targetIdx) {
        UInt32 nameLen = 0;
        if (!readLEUInt32(ptr, end, nameLen)) {
            return fail("MORP chunk truncated at target name length.");
        }

        MeshMorphTarget target;
        if (!readString(ptr, end, nameLen, target.name)) {
            return fail("MORP chunk truncated while reading target name.");
        }
        if (!readLEFloat32(ptr, end, target.defaultWeight)) {
            return fail("MORP defaultWeight is truncated or non-finite.");
        }

        UInt32 deltaCount = 0;
        UInt32 payloadChannels = 0;
        if (!readLEUInt32(ptr, end, deltaCount)) {
            return fail("MORP chunk truncated at deltaCount.");
        }
        if (!readLEUInt32(ptr, end, payloadChannels)) {
            return fail("MORP chunk truncated at payloadChannels.");
        }
        if ((payloadChannels & ~static_cast<UInt32>(kMeshMorphKnownPayloadChannels)) != 0u) {
            return fail("MORP target declares unknown payload channels.");
        }
        target.payloadChannels = static_cast<UInt8>(payloadChannels);

        const size_t bytesPerDelta = deltaByteSize(target.payloadChannels);
        const size_t remaining = static_cast<size_t>(end - ptr);
        if (deltaCount > remaining / bytesPerDelta) {
            return fail("MORP deltaCount exceeds the payload size.");
        }
        target.deltas.reserve(deltaCount);

        for (UInt32 deltaIdx = 0; deltaIdx < deltaCount; ++deltaIdx) {
            MeshMorphVertexDelta delta;
            delta.payloadChannels = target.payloadChannels;
            if (!readLEUInt32(ptr, end, delta.vertexIndex)) {
                return fail("MORP chunk truncated while reading vertexIndex.");
            }
            if (delta.vertexIndex >= mesh.getVertexCount()) {
                return fail("MORP vertexIndex is outside the base mesh.");
            }

            if ((delta.payloadChannels & kMeshMorphPayloadPosition) != 0u) {
                for (UInt32 ci = 0; ci < 3u; ++ci) {
                    if (!readLEFloat32(ptr, end, delta.positionDelta[ci])) {
                        return fail("MORP position delta is truncated or non-finite.");
                    }
                }
            }
            if ((delta.payloadChannels & kMeshMorphPayloadNormal) != 0u) {
                for (UInt32 ci = 0; ci < 3u; ++ci) {
                    if (!readLEFloat32(ptr, end, delta.normalDelta[ci])) {
                        return fail("MORP normal delta is truncated or non-finite.");
                    }
                }
            }
            if ((delta.payloadChannels & kMeshMorphPayloadTangent) != 0u) {
                for (UInt32 ci = 0; ci < 4u; ++ci) {
                    if (!readLEFloat32(ptr, end, delta.tangentDelta[ci])) {
                        return fail("MORP tangent delta is truncated or non-finite.");
                    }
                }
            }

            target.deltas.push_back(std::move(delta));
        }
        parsed.targets.push_back(std::move(target));
    }

    if (ptr != end) {
        return fail("MORP chunk has trailing bytes.");
    }

    out = std::move(parsed);
    return true;
}

bool buildMeshMorphContractSummary(const MeshMorphContract& in, MeshMorphContractSummary& out)
{
    out = MeshMorphContractSummary{};
    if (in.targets.empty()) return true;
    if (in.targets.size() > std::numeric_limits<UInt32>::max()) return false;

    out.hasMorphTargets = true;
    out.targetCount = static_cast<UInt32>(in.targets.size());
    UInt32 maxVertexIndex = 0;
    bool hasAnyDelta = false;
    for (const MeshMorphTarget& target : in.targets) {
        if (target.deltas.size()
            > static_cast<size_t>(std::numeric_limits<UInt32>::max() - out.totalDeltaCount)) {
            return false;
        }
        out.totalDeltaCount += static_cast<UInt32>(target.deltas.size());
        out.payloadChannels |= target.payloadChannels;
        for (const MeshMorphVertexDelta& delta : target.deltas) {
            hasAnyDelta = true;
            maxVertexIndex = std::max(maxVertexIndex, delta.vertexIndex);
            out.payloadChannels |= delta.payloadChannels;
        }
    }

    if (hasAnyDelta) {
        if (maxVertexIndex == std::numeric_limits<UInt32>::max()) return false;
        out.morphVertexCount = maxVertexIndex + 1u;
    }
    return true;
}

} // namespace ayt::resource
