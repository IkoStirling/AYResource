#include "Converter\MeshConverter.h"
#include "Loader\MeshLoader.h"
#include "AYMesh.h"
#include "IAYMesh.h"
#include "AYFile.h"
#include <AYGuid.h>
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

bool MeshConverter::saveToBinary(const MeshData& mesh, std::vector<UInt8>& outData) {
    bool hasSkinWeights = (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::SkinWeight))) != 0;

    UInt8 vertexStride = computeVertexStride(mesh.attributeMask);
    UInt32 vertexCount = static_cast<UInt32>(mesh.positions.size() / 3);
    UInt32 indexCount = static_cast<UInt32>(mesh.indices.size());
    UInt32 vertexDataSize = vertexCount * vertexStride;
    UInt32 indexDataSize = indexCount * sizeof(UInt32);
    UInt32 materialSlotsSize = 0;
    for (const auto& slot : mesh.materialSlots) {
        materialSlotsSize += sizeof(UInt32) + static_cast<UInt32>(slot.size());
    }
    UInt32 submeshesSize = static_cast<UInt32>(mesh.submeshes.size()) * sizeof(UInt32) * 3;
    UInt32 skinWeightsSize = hasSkinWeights ? vertexCount * sizeof(VertexSkinWeight) : 0;

    size_t dataSize = vertexDataSize + indexDataSize + materialSlotsSize + submeshesSize + skinWeightsSize;

    // 构建内容数据
    std::vector<UInt8> contentData(dataSize);
    UInt8* ptr = contentData.data();

    // 构建交错顶点数据
    UInt8 posOffset = 0, normOffset = 0, uvOffset = 0, tanOffset = 0, colOffset = 0;
    UInt8 cur = 0;
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Position))) { posOffset = cur; cur += 12; }
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Normal))) { normOffset = cur; cur += 12; }
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::UV))) { uvOffset = cur; cur += 8; }
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Tangent))) { tanOffset = cur; cur += 16; }
    if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Color))) { colOffset = cur; cur += 16; }

    UInt8 floatStride = cur / sizeof(Float32);
    std::vector<float> interleaved(floatStride * vertexCount, 0.0f);
    for (UInt32 v = 0; v < vertexCount; v++) {
        UInt32 base = v * floatStride;
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Position))) {
            UInt32 idx = base + posOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.positions[v * 3 + 0];
            interleaved[idx + 1] = mesh.positions[v * 3 + 1];
            interleaved[idx + 2] = mesh.positions[v * 3 + 2];
        }
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Normal))) {
            UInt32 idx = base + normOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.normals[v * 3 + 0];
            interleaved[idx + 1] = mesh.normals[v * 3 + 1];
            interleaved[idx + 2] = mesh.normals[v * 3 + 2];
        }
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::UV))) {
            UInt32 idx = base + uvOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.uvs[v * 2 + 0];
            interleaved[idx + 1] = mesh.uvs[v * 2 + 1];
        }
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Tangent))) {
            UInt32 idx = base + tanOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.tangents[v * 4 + 0];
            interleaved[idx + 1] = mesh.tangents[v * 4 + 1];
            interleaved[idx + 2] = mesh.tangents[v * 4 + 2];
            interleaved[idx + 3] = mesh.tangents[v * 4 + 3];
        }
        if (mesh.attributeMask & (1u << static_cast<uint8_t>(MeshAttribute::Color))) {
            UInt32 idx = base + colOffset / sizeof(Float32);
            interleaved[idx + 0] = mesh.colors[v * 4 + 0];
            interleaved[idx + 1] = mesh.colors[v * 4 + 1];
            interleaved[idx + 2] = mesh.colors[v * 4 + 2];
            interleaved[idx + 3] = mesh.colors[v * 4 + 3];
        }
    }
    memcpy(ptr, interleaved.data(), vertexDataSize);
    ptr += vertexDataSize;

    memcpy(ptr, mesh.indices.data(), indexDataSize);
    ptr += indexDataSize;

    for (const auto& slot : mesh.materialSlots) {
        UInt32 len = static_cast<UInt32>(slot.size());
        memcpy(ptr, &len, sizeof(UInt32)); ptr += sizeof(UInt32);
        memcpy(ptr, slot.data(), len); ptr += len;
    }

    for (const auto& submesh : mesh.submeshes) {
        const UInt32 indexOffset = submesh.startIndex;
        const UInt32 indexCount = submesh.indexCount;
        const UInt32 materialIndex = submesh.materialIndex;
        memcpy(ptr, &indexOffset, sizeof(UInt32)); ptr += sizeof(UInt32);
        memcpy(ptr, &indexCount, sizeof(UInt32)); ptr += sizeof(UInt32);
        memcpy(ptr, &materialIndex, sizeof(UInt32)); ptr += sizeof(UInt32);
    }

    if (hasSkinWeights && !mesh.skinWeights.empty()) {
        std::vector<VertexSkinWeight> packed(vertexCount);
        for (UInt32 v = 0; v < vertexCount; ++v) {
            const float* src = &mesh.skinWeights[v * 8];
            for (int b = 0; b < 4; ++b) {
                packed[v].boneIndex[b] = static_cast<UInt8>(src[b]);
                packed[v].boneWeight[b] = src[b + 4];
            }
        }
        memcpy(ptr, packed.data(), skinWeightsSize);
        ptr += skinWeightsSize;
    }

    // 计算 GUID
    lastGuid = ayt::storage::Guid::computeFromData(contentData.data(), contentData.size());

    // 构建 header
    MeshBinaryHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = IMesh::MAGIC;
    header.version = IMesh::VERSION;
    header.guid = lastGuid;
    header.attributeMask = mesh.attributeMask;
    header.flags = 0;
    header.vertexCount = vertexCount;
    header.indexCount = indexCount;
    header.submeshCount = static_cast<UInt32>(mesh.submeshes.size());
    header.materialSlotCount = static_cast<UInt32>(mesh.materialSlots.size());
    if (mesh.boundsMin[0] != mesh.boundsMax[0]) {
        header.boundsCenter[0] = (mesh.boundsMin[0] + mesh.boundsMax[0]) * 0.5f;
        header.boundsCenter[1] = (mesh.boundsMin[1] + mesh.boundsMax[1]) * 0.5f;
        header.boundsCenter[2] = (mesh.boundsMin[2] + mesh.boundsMax[2]) * 0.5f;
        header.boundsHalfExtent[0] = (mesh.boundsMax[0] - mesh.boundsMin[0]) * 0.5f;
        header.boundsHalfExtent[1] = (mesh.boundsMax[1] - mesh.boundsMin[1]) * 0.5f;
        header.boundsHalfExtent[2] = (mesh.boundsMax[2] - mesh.boundsMin[2]) * 0.5f;
        header.hasBounds = 1;
    } else {
        header.hasBounds = 0;
    }
    header.hasSkinWeights = hasSkinWeights ? 1 : 0;

    // 输出: header + contentData
    outData.resize(sizeof(MeshBinaryHeader) + dataSize);
    UInt8* outPtr = outData.data();
    memcpy(outPtr, &header, sizeof(header)); outPtr += sizeof(header);
    memcpy(outPtr, contentData.data(), dataSize);

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
