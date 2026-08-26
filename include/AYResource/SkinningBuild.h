#pragma once

#include "AYResource/IntermediateAsset.h"
#include <string>

namespace ayt::resource
{

// Backend cooking capability, not an asset/skeleton limit. The current
// uniform-buffer skinning path uses 128 matrices per draw; other cook
// profiles may select a different value up to the UInt8 vertex format limit.
struct SkinningBuildProfile {
    UInt32 maxBonesPerDraw = 128u;
    Float32 minimumInfluence = 0.001f;
};

struct SkinningBuildStats {
    UInt32 sourceSubmeshCount = 0u;
    UInt32 outputChunkCount = 0u;
    UInt32 splitSubmeshCount = 0u;
    UInt32 duplicatedVertexCount = 0u;
    UInt32 maximumPaletteSize = 0u;
};

// Converts global skeleton indices into per-submesh local palettes. A source
// submesh whose palette exceeds the profile is partitioned on triangle
// boundaries. The operation preserves material routing and duplicates sparse
// morph deltas for vertices copied into more than one render chunk.
bool buildSkinnedRenderChunks(const MeshData& source,
                              const SkinningBuildProfile& profile,
                              MeshData& output,
                              SkinningBuildStats* stats = nullptr,
                              std::string* error = nullptr);

} // namespace ayt::resource
