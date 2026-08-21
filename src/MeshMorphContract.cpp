#include "AYResource/MeshMorphContract.h"

#include <cstring>

namespace ayt::resource
{

namespace {

bool readLEUInt32(const UInt8*& ptr, const UInt8* end, UInt32& out)
{
    if (static_cast<size_t>(end - ptr) < sizeof(UInt32)) {
        return false;
    }
    std::memcpy(&out, ptr, sizeof(UInt32));
    ptr += sizeof(UInt32);
    return true;
}

bool readLEFloat32(const UInt8*& ptr, const UInt8* end, Float32& out)
{
    if (static_cast<size_t>(end - ptr) < sizeof(Float32)) {
        return false;
    }
    std::memcpy(&out, ptr, sizeof(Float32));
    ptr += sizeof(Float32);
    return true;
}

bool readString(const UInt8*& ptr,
               const UInt8* end,
               UInt32 length,
               std::string& out)
{
    if (length == 0) {
        out.clear();
        return true;
    }
    if (static_cast<size_t>(end - ptr) < length) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(ptr), length);
    ptr += length;
    return true;
}

}

bool readMeshMorphContract(const IMesh& mesh, MeshMorphContract& out, std::string* error)
{
    out = MeshMorphContract{};

    const IMesh::Extension* ext = mesh.findExtension(kMeshMorphChunkType);
    if (ext == nullptr || ext->size == 0 || ext->data == nullptr) {
        return true;
    }

    const UInt8* ptr = ext->data;
    const UInt8* end = ext->data + ext->size;

    UInt32 targetCount = 0;
    if (!readLEUInt32(ptr, end, targetCount)) {
        if (error != nullptr) {
            *error = "MORP chunk truncated at targetCount.";
        }
        return false;
    }

    out.targets.clear();
    out.targets.reserve(targetCount);

    for (UInt32 targetIdx = 0; targetIdx < targetCount; ++targetIdx) {
        UInt32 nameLen = 0;
        if (!readLEUInt32(ptr, end, nameLen)) {
            if (error != nullptr) {
                *error = "MORP chunk truncated at target name length.";
            }
            return false;
        }

        MeshMorphTarget target;
        if (!readString(ptr, end, nameLen, target.name)) {
            if (error != nullptr) {
                *error = "MORP chunk truncated while reading target name.";
            }
            return false;
        }

        if (!readLEFloat32(ptr, end, target.defaultWeight)) {
            if (error != nullptr) {
                *error = "MORP chunk truncated at defaultWeight.";
            }
            return false;
        }

        UInt32 deltaCount = 0;
        if (!readLEUInt32(ptr, end, deltaCount)) {
            if (error != nullptr) {
                *error = "MORP chunk truncated at deltaCount.";
            }
            return false;
        }

        UInt32 payloadChannels = 0;
        if (!readLEUInt32(ptr, end, payloadChannels)) {
            if (error != nullptr) {
                *error = "MORP chunk truncated at payloadChannels.";
            }
            return false;
        }
        target.payloadChannels = static_cast<UInt8>(payloadChannels & 0xFFu);

        target.deltas.reserve(deltaCount);

        for (UInt32 deltaIdx = 0; deltaIdx < deltaCount; ++deltaIdx) {
            MeshMorphVertexDelta delta;
            delta.payloadChannels = target.payloadChannels;

            if (!readLEUInt32(ptr, end, delta.vertexIndex)) {
                if (error != nullptr) {
                    *error = "MORP chunk truncated while reading vertexIndex.";
                }
                return false;
            }

            if (delta.payloadChannels & kMeshMorphPayloadPosition) {
                for (UInt32 ci = 0; ci < 3u; ++ci) {
                    if (!readLEFloat32(ptr, end, delta.positionDelta[ci])) {
                        if (error != nullptr) {
                            *error = "MORP chunk truncated while reading position delta.";
                        }
                        return false;
                    }
                }
            }

            if (delta.payloadChannels & kMeshMorphPayloadNormal) {
                for (UInt32 ci = 0; ci < 3u; ++ci) {
                    if (!readLEFloat32(ptr, end, delta.normalDelta[ci])) {
                        if (error != nullptr) {
                            *error = "MORP chunk truncated while reading normal delta.";
                        }
                        return false;
                    }
                }
            }

            if (delta.payloadChannels & kMeshMorphPayloadTangent) {
                for (UInt32 ci = 0; ci < 4u; ++ci) {
                    if (!readLEFloat32(ptr, end, delta.tangentDelta[ci])) {
                        if (error != nullptr) {
                            *error = "MORP chunk truncated while reading tangent delta.";
                        }
                        return false;
                    }
                }
            }

            target.deltas.push_back(delta);
        }

        out.targets.push_back(std::move(target));
    }

    if (ptr != end) {
        // Keep strictness: reject trailing padding to prevent accidental parser
        // drift from malformed exporters.
        if (error != nullptr) {
            *error = "MORP chunk has trailing bytes.";
        }
        return false;
    }

    return true;
}

bool buildMeshMorphContractSummary(const MeshMorphContract& in, MeshMorphContractSummary& out)
{
    out = MeshMorphContractSummary{};
    if (in.targets.empty()) {
        return true;
    }

    out.hasMorphTargets = true;
    out.targetCount = static_cast<UInt32>(in.targets.size());

    for (const MeshMorphTarget& target : in.targets) {
        out.totalDeltaCount += static_cast<UInt32>(target.deltas.size());
        out.payloadChannels |= target.payloadChannels;
        bool hasAnyDelta = false;
        for (const MeshMorphVertexDelta& delta : target.deltas) {
            if (!hasAnyDelta) {
                hasAnyDelta = true;
            }
            if (delta.vertexIndex >= out.morphVertexCount) {
                out.morphVertexCount = delta.vertexIndex;
            }
            out.payloadChannels |= delta.payloadChannels;
        }
        if (!hasAnyDelta && target.payloadChannels != 0u) {
            // Keep payload channel declaration visible even on degenerate empty
            // morph targets (should rarely happen, but safer for diagnostics).
            out.hasMorphTargets = true;
        }
    }

    if (out.totalDeltaCount > 0u) {
        // Convert max index into a count-like upper bound for diagnostics.
        ++out.morphVertexCount;
    }

    return true;
}

} // namespace ayt::resource
