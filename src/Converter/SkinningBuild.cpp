#include "AYResource/SkinningBuild.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace ayt::resource
{
namespace
{

void setError(std::string* error, const std::string& message)
{
    if (error) *error = message;
}

template <typename T>
void copyTuple(const std::vector<T>& source,
               UInt32 sourceVertex,
               UInt32 width,
               std::vector<T>& destination)
{
    if (source.empty()) return;
    const size_t begin = static_cast<size_t>(sourceVertex) * width;
    destination.insert(destination.end(), source.begin() + begin,
                       source.begin() + begin + width);
}

struct PendingChunk {
    const SubmeshData* sourceSubmesh = nullptr;
    std::vector<UInt32> sourceIndices;
    std::vector<UInt32> palette;
    std::unordered_set<UInt32> paletteSet;
};

} // namespace

bool buildSkinnedRenderChunks(const MeshData& source,
                              const SkinningBuildProfile& profile,
                              MeshData& output,
                              SkinningBuildStats* stats,
                              std::string* error)
{
    SkinningBuildStats localStats{};
    if (stats) *stats = localStats;
    if (error) error->clear();

    if (source.skinVertices.empty()) {
        output = source;
        return true;
    }
    if (profile.maxBonesPerDraw == 0u || profile.maxBonesPerDraw > 256u) {
        setError(error, "skinning profile maxBonesPerDraw must be in [1, 256]");
        return false;
    }
    if (!std::isfinite(profile.minimumInfluence) || profile.minimumInfluence < 0.0f) {
        setError(error, "skinning profile minimumInfluence must be finite and non-negative");
        return false;
    }
    if (source.skinIndexSpace == SkinIndexSpace::LocalPalette) {
        output = source;
        return true;
    }

    const UInt32 vertexCount = static_cast<UInt32>(source.positions.size() / 3u);
    if (vertexCount == 0u || source.positions.size() != static_cast<size_t>(vertexCount) * 3u
        || source.skinVertices.size() != vertexCount) {
        setError(error, "skin vertex count does not match the position stream");
        return false;
    }
    auto validStream = [vertexCount](size_t size, UInt32 width) {
        return size == 0u || size == static_cast<size_t>(vertexCount) * width;
    };
    if (!validStream(source.normals.size(), 3u)
        || !validStream(source.uvs.size(), 2u)
        || !validStream(source.tangents.size(), 4u)
        || !validStream(source.colors.size(), 4u)) {
        setError(error, "one or more vertex streams do not match the position count");
        return false;
    }

    std::vector<SubmeshData> implicitSubmeshes;
    const std::vector<SubmeshData>* sourceSubmeshes = &source.submeshes;
    if (sourceSubmeshes->empty()) {
        SubmeshData whole;
        whole.indexCount = static_cast<UInt32>(source.indices.size());
        implicitSubmeshes.push_back(whole);
        sourceSubmeshes = &implicitSubmeshes;
    }

    output = MeshData{};
    output.name = source.name;
    output.materialSlots = source.materialSlots;
    output.attributeMask = source.attributeMask;
    output.skinIndexSpace = SkinIndexSpace::LocalPalette;
    std::copy(std::begin(source.boundsMin), std::end(source.boundsMin), output.boundsMin);
    std::copy(std::begin(source.boundsMax), std::end(source.boundsMax), output.boundsMax);

    std::vector<std::vector<UInt32>> duplicatedVertices(vertexCount);

    auto emitChunk = [&](PendingChunk& pending) -> bool {
        if (pending.sourceIndices.empty()) return true;

        std::unordered_map<UInt32, UInt32> globalToLocal;
        globalToLocal.reserve(pending.palette.size());
        for (UInt32 i = 0; i < pending.palette.size(); ++i) {
            globalToLocal.emplace(pending.palette[i], i);
        }

        SubmeshData chunk = *pending.sourceSubmesh;
        chunk.startIndex = static_cast<UInt32>(output.indices.size());
        chunk.indexCount = static_cast<UInt32>(pending.sourceIndices.size());
        chunk.vertexOffset = static_cast<UInt32>(output.positions.size() / 3u);
        chunk.bonePalette = pending.palette;

        std::unordered_map<UInt32, UInt32> vertexMap;
        vertexMap.reserve(pending.sourceIndices.size());
        for (UInt32 sourceVertex : pending.sourceIndices) {
            if (sourceVertex >= vertexCount) {
                setError(error, "submesh index references a vertex outside the mesh");
                return false;
            }
            auto found = vertexMap.find(sourceVertex);
            UInt32 destinationVertex = 0u;
            if (found == vertexMap.end()) {
                destinationVertex = static_cast<UInt32>(output.positions.size() / 3u);
                vertexMap.emplace(sourceVertex, destinationVertex);
                copyTuple(source.positions, sourceVertex, 3u, output.positions);
                copyTuple(source.normals, sourceVertex, 3u, output.normals);
                copyTuple(source.uvs, sourceVertex, 2u, output.uvs);
                copyTuple(source.tangents, sourceVertex, 4u, output.tangents);
                copyTuple(source.colors, sourceVertex, 4u, output.colors);

                SkinVertexData skin = source.skinVertices[sourceVertex];
                Float32 retainedWeight = 0.0f;
                for (UInt32 slot = 0; slot < 4u; ++slot) {
                    if (!std::isfinite(skin.weight[slot]) || skin.weight[slot] < 0.0f) {
                        setError(error, "skin influence weights must be finite and non-negative");
                        return false;
                    }
                    if (skin.weight[slot] <= profile.minimumInfluence) {
                        skin.joint[slot] = 0u;
                        skin.weight[slot] = 0.0f;
                        continue;
                    }
                    const auto local = globalToLocal.find(skin.joint[slot]);
                    if (local == globalToLocal.end()) {
                        setError(error, "active vertex influence is absent from its chunk palette");
                        return false;
                    }
                    skin.joint[slot] = local->second;
                    retainedWeight += skin.weight[slot];
                }
                if (retainedWeight <= 0.0f) {
                    setError(error, "vertex has no influence above the cooking threshold");
                    return false;
                }
                for (UInt32 slot = 0; slot < 4u; ++slot) {
                    skin.weight[slot] /= retainedWeight;
                }
                output.skinVertices.push_back(skin);
                duplicatedVertices[sourceVertex].push_back(destinationVertex);
            } else {
                destinationVertex = found->second;
            }
            output.indices.push_back(destinationVertex);
        }

        output.submeshes.push_back(std::move(chunk));
        localStats.maximumPaletteSize = std::max(
            localStats.maximumPaletteSize,
            static_cast<UInt32>(pending.palette.size()));
        ++localStats.outputChunkCount;
        return true;
    };

    for (const SubmeshData& submesh : *sourceSubmeshes) {
        ++localStats.sourceSubmeshCount;
        const uint64_t end = static_cast<uint64_t>(submesh.startIndex) + submesh.indexCount;
        if (end > source.indices.size() || (submesh.indexCount % 3u) != 0u) {
            setError(error, "skinned submesh index range is invalid or not triangulated");
            return false;
        }

        const UInt32 chunksBefore = localStats.outputChunkCount;
        PendingChunk pending;
        pending.sourceSubmesh = &submesh;
        for (UInt32 at = submesh.startIndex; at < end; at += 3u) {
            std::array<UInt32, 12> triangleBones{};
            UInt32 triangleBoneCount = 0u;
            for (UInt32 corner = 0; corner < 3u; ++corner) {
                const UInt32 vertex = source.indices[at + corner];
                if (vertex >= vertexCount) {
                    setError(error, "triangle references a vertex outside the mesh");
                    return false;
                }
                const SkinVertexData& skin = source.skinVertices[vertex];
                for (UInt32 slot = 0; slot < 4u; ++slot) {
                    if (skin.weight[slot] <= profile.minimumInfluence) continue;
                    const UInt32 joint = skin.joint[slot];
                    if (std::find(triangleBones.begin(), triangleBones.begin() + triangleBoneCount,
                                  joint) == triangleBones.begin() + triangleBoneCount) {
                        triangleBones[triangleBoneCount++] = joint;
                    }
                }
            }
            if (triangleBoneCount > profile.maxBonesPerDraw) {
                setError(error, "one triangle exceeds the skinning palette capacity");
                return false;
            }

            UInt32 added = 0u;
            for (UInt32 i = 0; i < triangleBoneCount; ++i) {
                if (pending.paletteSet.find(triangleBones[i]) == pending.paletteSet.end()) ++added;
            }
            if (!pending.sourceIndices.empty()
                && pending.palette.size() + added > profile.maxBonesPerDraw) {
                if (!emitChunk(pending)) return false;
                pending = PendingChunk{};
                pending.sourceSubmesh = &submesh;
            }
            for (UInt32 i = 0; i < triangleBoneCount; ++i) {
                if (pending.paletteSet.insert(triangleBones[i]).second) {
                    pending.palette.push_back(triangleBones[i]);
                }
            }
            pending.sourceIndices.push_back(source.indices[at + 0u]);
            pending.sourceIndices.push_back(source.indices[at + 1u]);
            pending.sourceIndices.push_back(source.indices[at + 2u]);
        }
        if (!emitChunk(pending)) return false;
        if (localStats.outputChunkCount - chunksBefore > 1u) ++localStats.splitSubmeshCount;
    }

    for (const MorphTargetData& target : source.morphTargets) {
        MorphTargetData cooked;
        cooked.name = target.name;
        cooked.defaultWeight = target.defaultWeight;
        for (const MorphVertexDelta& delta : target.deltas) {
            if (delta.vertexIndex >= duplicatedVertices.size()) {
                setError(error, "morph target references a vertex outside the mesh");
                return false;
            }
            for (UInt32 destination : duplicatedVertices[delta.vertexIndex]) {
                MorphVertexDelta copy = delta;
                copy.vertexIndex = destination;
                cooked.deltas.push_back(copy);
            }
        }
        output.morphTargets.push_back(std::move(cooked));
    }

    const UInt32 outputVertexCount = static_cast<UInt32>(output.positions.size() / 3u);
    localStats.duplicatedVertexCount = outputVertexCount > vertexCount
        ? outputVertexCount - vertexCount : 0u;
    if (stats) *stats = localStats;
    return true;
}

} // namespace ayt::resource
