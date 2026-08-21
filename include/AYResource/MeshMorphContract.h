#pragma once

#include "AYResource/assetsDefs/IMesh.h"

#include <string>
#include <vector>

namespace ayt::resource
{

// Shape-key payload type emitted into IMesh extension chunk type.
inline constexpr UInt32 kMeshMorphChunkType = 0x50524F4D; // 'MORP'

// Bit mask for optional payload channels stored in each delta entry.
enum MeshMorphPayloadChannel : UInt8 {
    kMeshMorphPayloadPosition = 1u << 0,
    kMeshMorphPayloadNormal   = 1u << 1,
    kMeshMorphPayloadTangent  = 1u << 2,
};

struct MeshMorphVertexDelta {
    UInt32 vertexIndex = 0;
    Float32 positionDelta[3] = {0.0f, 0.0f, 0.0f};
    Float32 normalDelta[3] = {0.0f, 0.0f, 0.0f};
    Float32 tangentDelta[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    UInt8 payloadChannels = 0;
};

struct MeshMorphTarget {
    std::string name;
    Float32 defaultWeight = 0.0f;
    UInt8 payloadChannels = 0;
    std::vector<MeshMorphVertexDelta> deltas;
};

struct MeshMorphContract {
    UInt32 version = 1;
    std::vector<MeshMorphTarget> targets;
};

struct MeshMorphContractSummary {
    bool hasMorphTargets = false;
    UInt32 targetCount = 0;
    UInt32 totalDeltaCount = 0;
    UInt32 morphVertexCount = 0;
    UInt8 payloadChannels = 0;
};

bool readMeshMorphContract(const IMesh& mesh, MeshMorphContract& out, std::string* error = nullptr);
bool buildMeshMorphContractSummary(const MeshMorphContract& in, MeshMorphContractSummary& out);

inline bool readMeshMorphContractSummary(const IMesh& mesh,
                                        MeshMorphContractSummary& out,
                                        std::string* error = nullptr)
{
    MeshMorphContract contract;
    if (!readMeshMorphContract(mesh, contract, error)) {
        out = MeshMorphContractSummary{};
        return false;
    }
    return buildMeshMorphContractSummary(contract, out);
}

} // namespace ayt::resource
