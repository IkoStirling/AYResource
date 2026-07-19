#include "Converter\MeshConverter.h"
#include "Loader\MeshLoader.h"
#include "AYMesh.h"
#include "IAYMesh.h"
#include "ayio/File.h"
#include <aystorage/Guid.h>
#include <AYLog.h>
#include <vector>
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
bool MeshConverter::saveToBinary(const MeshData& mesh, std::vector<UInt8>& outData) {
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

    // skin weights: 8 floats per vertex = (4 indices as float, 4 weights)
    const bool hasSkin = (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::SkinWeight))) != 0
                        && !mesh.skinWeights.empty();
    if (hasSkin) {
        std::vector<VertexSkinWeight> packed(vertexCount);
        for (UInt32 v = 0; v < vertexCount; ++v) {
            const float* src = &mesh.skinWeights[v * 8];
            for (int b = 0; b < 4; ++b) {
                packed[v].boneIndex[b]  = static_cast<UInt8>(src[b]);
                packed[v].boneWeight[b] = src[b + 4];
            }
        }
        tmp._setForTestSkinWeights(packed);
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
            lastOutputPath = virtualPath;
            return true;
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
                // 检查文件大小是否匹配，如果不一致说明格式已更新，需要重新生成
                ayt::io::File oldFile(fullPath, ayt::io::File::Mode::BinaryRead);
                if (oldFile.isOpen() && static_cast<size_t>(oldFile.size()) == binaryData.size()) {
                    ayt::log::debug("[MeshConverter] SKIP %s (size match)", name.c_str());
                } else {
                    ayt::log::info("[MeshConverter] REPLACE %s (size changed or file missing)", name.c_str());
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
        res.size = static_cast<int64_t>(binaryData.size());
        results.push_back(res);
        lastOutputPath = virtualPath;
    }

    return results;
}

} // namespace ayt::resource
