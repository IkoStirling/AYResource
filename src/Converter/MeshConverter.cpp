#include "AYResource/Converter/MeshConverter.h"
#include "AYResource/Loader/MeshLoader.h"
#include "AYResource/assetsImpl/Mesh.h"
#include "AYResource/MeshMorphContract.h"
#include "AYResource/assetsDefs/IMesh.h"
#include "AYResource/SkinningBuild.h"
#include "AYIO/File.h"
#include <AYStorage/Guid.h>
#include <AYLog.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

namespace ayt::resource
{

UInt8 MeshConverter::computeVertexStride(uint8_t attributeMask) {
    UInt8 stride = 0;
    if (attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Position))) stride += 12;
    if (attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Normal))) stride += 12;
    if (attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::UV))) stride += 8;
    if (attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Tangent))) stride += 16;
    if (attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Color))) stride += 16;
    return stride;
}

// MeshConverter::saveToBinary — v1 chunked layout
//
// 复用 Mesh 类的 chunked 序列化：把 MeshData 数据装到一个临时 Mesh 实例上，
// 调用 Mesh::saveToBinary，再将生成的 chunked 字节流写出。这样保证两个写入端
// (Mesh::saveToBinary 和 MeshConverter) 共用同一份磁盘格式，未来修改一处即可。
bool MeshConverter::saveToBinary(const MeshData& source, std::vector<UInt8>& outData) {
    MeshData cooked;
    const MeshData* input = &source;
    if (!source.skinVertices.empty()
        && source.skinIndexSpace == SkinIndexSpace::GlobalSkeleton) {
        SkinningBuildStats stats;
        std::string error;
        if (!buildSkinnedRenderChunks(source, skinningProfile, cooked, &stats, &error)) {
            ayt::log::error("[MeshConverter] skin cooking failed for '%s': %s",
                            source.name.c_str(), error.c_str());
            return false;
        }
        ayt::log::info("[MeshConverter] skin cooked '%s': sections=%u chunks=%u "
                       "split=%u duplicatedVertices=%u maxPalette=%u",
                       source.name.c_str(), stats.sourceSubmeshCount,
                       stats.outputChunkCount, stats.splitSubmeshCount,
                       stats.duplicatedVertexCount, stats.maximumPaletteSize);
        input = &cooked;
    }
    const MeshData& mesh = *input;
    const UInt32 vertexCount = static_cast<UInt32>(mesh.positions.size() / 3);
    const UInt32 indexCount  = static_cast<UInt32>(mesh.indices.size());

    if (vertexCount == 0 || indexCount == 0) {
        ayt::log::error("[MeshConverter] empty MeshData (verts=%u indices=%u)", vertexCount, indexCount);
        return false;
    }

    // 构造临时 Mesh
    Mesh tmp;
    tmp.setGuid(ayt::storage::Guid::computeFromData(nullptr, 0)); // 覆盖为 content-hash 后再 set

    // attribute mask
    tmp._setForTestAttributeMask(mesh.attributeMask);

    // 填充 interleaved vertex data
    const UInt8 attrStride = computeVertexStride(mesh.attributeMask);
    tmp._setForTestVertexLayout(mesh.attributeMask, vertexCount, attrStride);

    // 拷贝 interleaved vertex stream
    std::vector<Float32> interleaved(static_cast<size_t>(attrStride / sizeof(Float32)) * vertexCount, 0.0f);
    UInt8 posOffset = 0, normOffset = 0, uvOffset = 0, tanOffset = 0, colOffset = 0;
    UInt8 cur = 0;
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Position))) { posOffset = cur; cur += 12; }
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Normal))) { normOffset = cur; cur += 12; }
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::UV))) { uvOffset = cur; cur += 8; }
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Tangent))) { tanOffset = cur; cur += 16; }
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Color))) { colOffset = cur; cur += 16; }
    for (UInt32 v = 0; v < vertexCount; ++v) {
        const UInt32 base = v * (attrStride / sizeof(Float32));
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Position))) {
            const UInt32 idx = base + posOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.positions[v * 3 + 0];
            interleaved[idx + 1] = mesh.positions[v * 3 + 1];
            interleaved[idx + 2] = mesh.positions[v * 3 + 2];
        }
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Normal))) {
            const UInt32 idx = base + normOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.normals[v * 3 + 0];
            interleaved[idx + 1] = mesh.normals[v * 3 + 1];
            interleaved[idx + 2] = mesh.normals[v * 3 + 2];
        }
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::UV))) {
            const UInt32 idx = base + uvOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.uvs[v * 2 + 0];
            interleaved[idx + 1] = mesh.uvs[v * 2 + 1];
        }
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Tangent))) {
            const UInt32 idx = base + tanOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.tangents[v * 4 + 0];
            interleaved[idx + 1] = mesh.tangents[v * 4 + 1];
            interleaved[idx + 2] = mesh.tangents[v * 4 + 2];
            interleaved[idx + 3] = mesh.tangents[v * 4 + 3];
        }
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Color))) {
            const UInt32 idx = base + colOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.colors[v * 4 + 0];
            interleaved[idx + 1] = mesh.colors[v * 4 + 1];
            interleaved[idx + 2] = mesh.colors[v * 4 + 2];
            interleaved[idx + 3] = mesh.colors[v * 4 + 3];
        }
    }
    tmp._setForTestVertexData(interleaved.data(), interleaved.size() * sizeof(Float32));

    // indices
    tmp._setForTestIndices(mesh.indices.data(), indexCount);

    // submeshes (SubmeshData → IMesh::Submesh, 字段对齐：startIndex → indexOffset)
    // F-01: vertexOffset 透传 (Phase 0 之前被丢弃, R-02 关闭)
    std::vector<IMesh::Submesh> submeshVec(mesh.submeshes.size());
    for (size_t i = 0; i < mesh.submeshes.size(); ++i) {
        submeshVec[i].indexOffset   = mesh.submeshes[i].startIndex;
        submeshVec[i].indexCount    = mesh.submeshes[i].indexCount;
        submeshVec[i].materialIndex = mesh.submeshes[i].materialIndex;
        submeshVec[i].vertexOffset  = mesh.submeshes[i].vertexOffset;
    }
    tmp._setForTestSubmeshes(submeshVec.data(), static_cast<UInt32>(submeshVec.size()));

    // material slots
    for (const auto& slot : mesh.materialSlots) {
        tmp._addForTestMaterialSlot(slot);
    }

    // Skin vertices are already draw-local after buildSkinnedRenderChunks.
    const bool hasSkin = (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::SkinWeight))) != 0
                        && !mesh.skinVertices.empty();
    if (hasSkin) {
        if (mesh.skinIndexSpace != SkinIndexSpace::LocalPalette
            || mesh.skinVertices.size() != vertexCount
            || mesh.submeshes.size() == 0u) {
            ayt::log::error("[MeshConverter] invalid cooked skin contract for '%s'", mesh.name.c_str());
            return false;
        }
        for (size_t sectionIndex = 0; sectionIndex < mesh.submeshes.size(); ++sectionIndex) {
            const SubmeshData& submesh = mesh.submeshes[sectionIndex];
            if (submesh.bonePalette.size() > skinningProfile.maxBonesPerDraw
                || submesh.bonePalette.size() > 256u) {
                ayt::log::error("[MeshConverter] section %zu palette exceeds cook profile", sectionIndex);
                return false;
            }
            const uint64_t rangeEnd = static_cast<uint64_t>(submesh.startIndex)
                                    + submesh.indexCount;
            if (rangeEnd > mesh.indices.size()) {
                ayt::log::error("[MeshConverter] section %zu index range is invalid", sectionIndex);
                return false;
            }
            for (uint64_t at = submesh.startIndex; at < rangeEnd; ++at) {
                const UInt32 vertex = mesh.indices[at];
                if (vertex >= vertexCount) {
                    ayt::log::error("[MeshConverter] section %zu references vertex %u/%u",
                                    sectionIndex, vertex, vertexCount);
                    return false;
                }
                const SkinVertexData& skin = mesh.skinVertices[vertex];
                Float32 weightSum = 0.0f;
                for (UInt32 slot = 0u; slot < 4u; ++slot) {
                    const Float32 weight = skin.weight[slot];
                    if (!std::isfinite(weight) || weight < 0.0f
                        || (weight > 0.0f && skin.joint[slot] >= submesh.bonePalette.size())) {
                        ayt::log::error("[MeshConverter] section %zu has invalid local skin slot", sectionIndex);
                        return false;
                    }
                    weightSum += weight;
                }
                if (std::abs(weightSum - 1.0f) > 0.001f) {
                    ayt::log::error("[MeshConverter] section %zu has non-normalized skin weights", sectionIndex);
                    return false;
                }
            }
        }
        std::vector<VertexSkinWeight> packed(vertexCount);
        for (UInt32 v = 0; v < vertexCount; ++v) {
            for (int b = 0; b < 4; ++b) {
                const UInt32 localJoint = mesh.skinVertices[v].joint[b];
                const Float32 weight = mesh.skinVertices[v].weight[b];
                if (localJoint > 255u && weight > 0.0f) {
                    ayt::log::error("[MeshConverter] local skin index %u exceeds UInt8", localJoint);
                    return false;
                }
                // GPU array indexing may still evaluate a zero-weight slot,
                // so canonicalize inactive indices instead of narrowing junk.
                packed[v].boneIndex[b] = weight > 0.0f
                    ? static_cast<UInt8>(localJoint) : 0u;
                packed[v].boneWeight[b] = weight;
            }
        }
        tmp._setForTestSkinWeights(packed);

        std::vector<SkinPalette> palettes;
        std::vector<UInt32> paletteJoints;
        palettes.reserve(mesh.submeshes.size());
        for (const SubmeshData& submesh : mesh.submeshes) {
            SkinPalette palette;
            palette.jointOffset = static_cast<UInt32>(paletteJoints.size());
            palette.jointCount = static_cast<UInt32>(submesh.bonePalette.size());
            palettes.push_back(palette);
            paletteJoints.insert(paletteJoints.end(), submesh.bonePalette.begin(),
                                 submesh.bonePalette.end());
        }
        tmp._setForTestSkinPalettes(palettes, paletteJoints);
    }

    // bounds
    if (mesh.boundsMin[0] != mesh.boundsMax[0]) {
        ayt::math::FVector3 c{
            (mesh.boundsMin[0] + mesh.boundsMax[0]) * 0.5f,
            (mesh.boundsMin[1] + mesh.boundsMax[1]) * 0.5f,
            (mesh.boundsMin[2] + mesh.boundsMax[2]) * 0.5f
        };
        ayt::math::FVector3 he{
            (mesh.boundsMax[0] - mesh.boundsMin[0]) * 0.5f,
            (mesh.boundsMax[1] - mesh.boundsMin[1]) * 0.5f,
            (mesh.boundsMax[2] - mesh.boundsMin[2]) * 0.5f
        };
        tmp._setForTestBounds(c, he);
    }

    // MORP extension (runtime contract, no rendering side effects yet).
    if (!mesh.morphTargets.empty()) {
        const UInt8 hasPos =
            (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Position))) != 0;
        const UInt8 hasNormal =
            (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Normal))) != 0;
        const UInt8 hasTangent =
            (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Tangent))) != 0;

        auto writeU32 = [](std::vector<UInt8>& out, UInt32 v) {
            const size_t pos = out.size();
            out.resize(pos + sizeof(UInt32));
            std::memcpy(out.data() + pos, &v, sizeof(UInt32));
        };
        auto writeF32 = [](std::vector<UInt8>& out, Float32 v) {
            const size_t pos = out.size();
            out.resize(pos + sizeof(Float32));
            std::memcpy(out.data() + pos, &v, sizeof(Float32));
        };

        std::vector<UInt8> morphPayload;
        writeU32(morphPayload, kMeshMorphPayloadMagic);
        writeU32(morphPayload, kMeshMorphCurrentVersion);
        writeU32(morphPayload, 0u); // flags
        writeU32(morphPayload, static_cast<UInt32>(mesh.morphTargets.size()));
        for (const auto& target : mesh.morphTargets) {
            const std::string& name = target.name;
            const UInt32 nameLen = static_cast<UInt32>(name.size());
            UInt32 attributeMask = 0u;
            if (hasPos) attributeMask |= kMeshMorphPayloadPosition;
            if (hasNormal) attributeMask |= kMeshMorphPayloadNormal;
            if (hasTangent) attributeMask |= kMeshMorphPayloadTangent;

            writeU32(morphPayload, nameLen);
            if (nameLen > 0) {
                const size_t pos = morphPayload.size();
                morphPayload.resize(pos + nameLen);
                std::memcpy(morphPayload.data() + pos, name.data(), nameLen);
            }
            writeF32(morphPayload, target.defaultWeight);
            writeU32(morphPayload, static_cast<UInt32>(target.deltas.size()));
            writeU32(morphPayload, attributeMask);

            for (const auto& delta : target.deltas) {
                writeU32(morphPayload, delta.vertexIndex);
                if (hasPos) {
                    writeF32(morphPayload, delta.positionDelta[0]);
                    writeF32(morphPayload, delta.positionDelta[1]);
                    writeF32(morphPayload, delta.positionDelta[2]);
                }
                if (hasNormal) {
                    writeF32(morphPayload, delta.normalDelta[0]);
                    writeF32(morphPayload, delta.normalDelta[1]);
                    writeF32(morphPayload, delta.normalDelta[2]);
                }
                if (hasTangent) {
                    writeF32(morphPayload, delta.tangentDelta[0]);
                    writeF32(morphPayload, delta.tangentDelta[1]);
                    writeF32(morphPayload, delta.tangentDelta[2]);
                    writeF32(morphPayload, delta.tangentDelta[3]);
                }
            }
        }

        tmp._setForTestExtensionBytes(MeshChunkFourCC::MORP, morphPayload);
    }

    // 调 Mesh 自带的 chunked saveToBinary
    if (!tmp.saveToBinary(outData)) {
        return false;
    }

    // 用 content 重新计算 GUID（覆盖 header.guid 字节）
    lastGuid = ayt::storage::Guid::computeFromData(outData.data() + sizeof(MeshBinaryHeader),
                                                    outData.size() - sizeof(MeshBinaryHeader));
    MeshBinaryHeader header;
    std::memcpy(&header, outData.data(), sizeof(header));
    header.guid = lastGuid;
    std::memcpy(outData.data(), &header, sizeof(header));

    return true;
}

bool MeshConverter::convert(const MeshData& mesh) {
    std::vector<UInt8> binaryData;
    if (!saveToBinary(mesh, binaryData)) {
        return false;
    }

    if (!outputDir.empty() && !virtualPath.empty()) {
        std::string fullPath = outputDir + "/" + virtualPath;
        if (ayt::io::File::exists(fullPath)) {
            const std::vector<uint8_t> oldData =
                ayt::io::File::readAllBytes(fullPath);
            if (oldData == binaryData) {
                lastOutputPath = virtualPath;
                return true;
            }
        }
        if (!writeFile(fullPath, binaryData.data(), binaryData.size())) {
            return false;
        }
        lastOutputPath = virtualPath;
    }

    return true;
}

std::vector<ConversionResult::ConvertedResource> MeshConverter::convertAll(
    const std::vector<MeshData>& meshes,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (size_t i = 0; i < meshes.size(); i++) {
        const auto& mesh = meshes[i];
        std::string safeName = mesh.name;
        size_t pos;
        while ((pos = safeName.find('/')) != std::string::npos) safeName.replace(pos, 1, "_");
        while ((pos = safeName.find('\\')) != std::string::npos) safeName.replace(pos, 1, "_");

        std::string name = safeName.empty()
            ? baseName + "_" + std::to_string(i) + ".aymesh"
            : baseName + "_" + safeName + ".aymesh";

        virtualPath = "meshes/" + name;

        std::vector<UInt8> binaryData;
        if (!saveToBinary(mesh, binaryData)) {
            continue;
        }

        if (!outputDir.empty()) {
            std::string fullPath = outputDir + "/" + virtualPath;
            if (ayt::io::File::exists(fullPath)) {
                // Equal size does not imply equal content: material indices,
                // vertex values and GUIDs can all change in-place. Compare the
                // canonical serialized bytes so a contract-triggered rebuild
                // cannot silently retain a stale mesh.
                const std::vector<uint8_t> oldData =
                    ayt::io::File::readAllBytes(fullPath);
                if (oldData == binaryData) {
                    ayt::log::debug("[MeshConverter] SKIP %s (content match)", name.c_str());
                } else {
                    ayt::log::info("[MeshConverter] REPLACE %s (content changed)", name.c_str());
                    writeFile(fullPath, binaryData.data(), binaryData.size());
                }
            } else {
                ayt::log::info("[MeshConverter] CREATE %s", name.c_str());
                writeFile(fullPath, binaryData.data(), binaryData.size());
            }
        }

        ConversionResult::ConvertedResource res;
        res.guid = lastGuid;
        res.path = virtualPath;
        res.type = "Mesh";
        res.role = mesh.skinVertices.empty() ? "StaticMesh" : "SkinnedMesh";
        res.size = static_cast<uint64_t>(binaryData.size());
        results.push_back(res);
        lastOutputPath = virtualPath;
    }

    return results;
}

} // namespace ayt::resource
