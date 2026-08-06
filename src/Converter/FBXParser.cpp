#include "Converter\FBXParser.h"
#include "AYVirtualAssetPath.h"
#include "IAYMesh.h"
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace ayt::resource
{

FBXParser::FBXParser(const std::string& sourcePath)
    : _sourcePath(sourcePath) {}

bool FBXParser::parse(const std::string& sourcePath) {
    if (!sourcePath.empty()) {
        _sourcePath = sourcePath;
    }

    if (_sourcePath.empty()) {
        return false;
    }

    Assimp::Importer importer;

    unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices;
    if (_loadOption == IConverter::LoadOption::MeshOnly) {
        // MeshOnly: 最小后处理
    } else {
        // Full: 保留所有数据 (R-02: 加 ValidateDataStructure 拦截坏骨骼/空轨道)
        flags |= aiProcess_GenSmoothNormals;
        flags |= aiProcess_CalcTangentSpace;
        flags |= aiProcess_ValidateDataStructure;
    }

    const aiScene* scene = importer.ReadFile(_sourcePath, flags);
    if (!scene) {
        return false;
    }

    if (!scene->mRootNode) {
        return false;
    }

    _result = std::make_unique<IntermediateAsset>();

    // 解析所有 Mesh - Submesh 永不分离，按顶级节点分组
    if (_separateModels) {
        _collectNodeMeshes(scene->mRootNode, scene, "");
    } else {
        _parseAllMeshesAsOne(scene);
    }

    // 解析所有 Material
    for (unsigned int i = 0; i < scene->mNumMaterials; i++) {
        if (scene->mMaterials[i]) {
            _parseMaterial(scene->mMaterials[i], i);
        }
    }

    // 解析所有 Texture (embedded)
    for (unsigned int i = 0; i < scene->mNumTextures; i++) {
        if (scene->mTextures[i]) {
            _parseTexture(scene->mTextures[i], i);
        }
    }

    // 解析所有 Skeleton
    _parseSkeletons(scene);

    // R-02: 解析 Animations (MeshOnly 跳过;Full 时全量提取)
    if (_loadOption != IConverter::LoadOption::MeshOnly) {
        _parseAnimations(scene);
    }

    return !_result->meshes.empty();
}

std::unique_ptr<IntermediateAsset> FBXParser::getResult() {
    return std::move(_result);
}

// 辅助函数：复制顶点属性数据
static void copyVertexAttribute(
    const aiMesh* m,
    MeshData& mesh,
    UInt8 attribute,
    UInt32 vertexOffset,
    UInt32 vertexCount,
    float boundsMin[3],
    float boundsMax[3]
) {
    switch (attribute) {
        case static_cast<UInt8>(MeshAttribute::Position):
            for (UInt32 v = 0; v < vertexCount; v++) {
                mesh.positions[(vertexOffset + v) * 3 + 0] = m->mVertices[v].x;
                mesh.positions[(vertexOffset + v) * 3 + 1] = m->mVertices[v].y;
                mesh.positions[(vertexOffset + v) * 3 + 2] = m->mVertices[v].z;
                boundsMin[0] = std::min(boundsMin[0], m->mVertices[v].x);
                boundsMin[1] = std::min(boundsMin[1], m->mVertices[v].y);
                boundsMin[2] = std::min(boundsMin[2], m->mVertices[v].z);
                boundsMax[0] = std::max(boundsMax[0], m->mVertices[v].x);
                boundsMax[1] = std::max(boundsMax[1], m->mVertices[v].y);
                boundsMax[2] = std::max(boundsMax[2], m->mVertices[v].z);
            }
            break;
        case static_cast<UInt8>(MeshAttribute::Normal):
            for (UInt32 v = 0; v < vertexCount; v++) {
                mesh.normals[(vertexOffset + v) * 3 + 0] = m->mNormals[v].x;
                mesh.normals[(vertexOffset + v) * 3 + 1] = m->mNormals[v].y;
                mesh.normals[(vertexOffset + v) * 3 + 2] = m->mNormals[v].z;
            }
            break;
        case static_cast<UInt8>(MeshAttribute::UV):
            for (UInt32 v = 0; v < vertexCount; v++) {
                mesh.uvs[(vertexOffset + v) * 2 + 0] = m->mTextureCoords[0][v].x;
                mesh.uvs[(vertexOffset + v) * 2 + 1] = m->mTextureCoords[0][v].y;
            }
            break;
        case static_cast<UInt8>(MeshAttribute::Tangent):
            for (UInt32 v = 0; v < vertexCount; v++) {
                mesh.tangents[(vertexOffset + v) * 4 + 0] = m->mTangents[v].x;
                mesh.tangents[(vertexOffset + v) * 4 + 1] = m->mTangents[v].y;
                mesh.tangents[(vertexOffset + v) * 4 + 2] = m->mTangents[v].z;
                float handedness = (m->mNormals[v] ^ m->mTangents[v]) * m->mBitangents[v] > 0 ? 1.0f : -1.0f;
                mesh.tangents[(vertexOffset + v) * 4 + 3] = handedness;
            }
            break;
        case static_cast<UInt8>(MeshAttribute::Color):
            for (UInt32 v = 0; v < vertexCount; v++) {
                mesh.colors[(vertexOffset + v) * 4 + 0] = m->mColors[0][v].r;
                mesh.colors[(vertexOffset + v) * 4 + 1] = m->mColors[0][v].g;
                mesh.colors[(vertexOffset + v) * 4 + 2] = m->mColors[0][v].b;
                mesh.colors[(vertexOffset + v) * 4 + 3] = m->mColors[0][v].a;
            }
            break;
    }
}

// 合并模式：将所有 aiMesh 合并为一个 MeshData
void FBXParser::_parseAllMeshesAsOne(const aiScene* scene) {
    if (!scene->mRootNode || scene->mNumMeshes == 0) return;

    MeshData mesh;
    mesh.name = "merged";

    UInt32 totalVertexCount = 0;
    UInt32 totalIndexCount = 0;
    UInt8 mergedAttributeMask = 0;

    // 第一遍：计算总大小
    for (unsigned int mi = 0; mi < scene->mNumMeshes; mi++) {
        const aiMesh* m = scene->mMeshes[mi];
        mergedAttributeMask |= _getMeshAttributeMask(m);
        totalVertexCount += m->mNumVertices;
        for (unsigned int f = 0; f < m->mNumFaces; f++) {
            totalIndexCount += m->mFaces[f].mNumIndices;
        }
    }

    // MeshOnly 模式：只保留 Position 和 UV，忽略 Normal/Tangent/Color
    if (_loadOption == IConverter::LoadOption::MeshOnly) {
        mergedAttributeMask &= (1u << static_cast<UInt8>(MeshAttribute::Position)) |
                               (1u << static_cast<UInt8>(MeshAttribute::UV));
    }

    mesh.attributeMask = mergedAttributeMask;

    // 分配内存
    mesh.positions.resize(totalVertexCount * 3);
    mesh.indices.resize(totalIndexCount);
    if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::Normal))) {
        mesh.normals.resize(totalVertexCount * 3);
    }
    if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::UV))) {
        mesh.uvs.resize(totalVertexCount * 2);
    }
    if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
        mesh.tangents.resize(totalVertexCount * 4);
    }
    if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::Color))) {
        mesh.colors.resize(totalVertexCount * 4);
    }
    if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::SkinWeight))) {
        mesh.skinWeights.resize(totalVertexCount * 8, 0.0f);  // 4 indices + 4 weights per vertex
        // 初始化：每个顶点的默认权重（如果没有骨骼影响）
        for (UInt32 v = 0; v < totalVertexCount; v++) {
            mesh.skinWeights[v * 8 + 0] = 0.0f;  // bone index 0
            mesh.skinWeights[v * 8 + 1] = 0.0f;  // bone index 1
            mesh.skinWeights[v * 8 + 2] = 0.0f;  // bone index 2
            mesh.skinWeights[v * 8 + 3] = 0.0f;  // bone index 3
            mesh.skinWeights[v * 8 + 4] = 1.0f;  // weight 0 = 1 (默认完全受骨骼0影响)
            mesh.skinWeights[v * 8 + 5] = 0.0f;  // weight 1
            mesh.skinWeights[v * 8 + 6] = 0.0f;  // weight 2
            mesh.skinWeights[v * 8 + 7] = 0.0f;  // weight 3
        }
    }

    float boundsMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float boundsMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    // 第二遍：复制数据
    UInt32 vertexOffset = 0;
    UInt32 indexOffset = 0;

    for (unsigned int mi = 0; mi < scene->mNumMeshes; mi++) {
        const aiMesh* m = scene->mMeshes[mi];
        UInt32 meshVertexCount = m->mNumVertices;

        // 复制顶点位置
        if (m->HasPositions()) {
            for (unsigned int v = 0; v < m->mNumVertices; v++) {
                mesh.positions[(vertexOffset + v) * 3 + 0] = m->mVertices[v].x;
                mesh.positions[(vertexOffset + v) * 3 + 1] = m->mVertices[v].y;
                mesh.positions[(vertexOffset + v) * 3 + 2] = m->mVertices[v].z;

                boundsMin[0] = std::min(boundsMin[0], m->mVertices[v].x);
                boundsMin[1] = std::min(boundsMin[1], m->mVertices[v].y);
                boundsMin[2] = std::min(boundsMin[2], m->mVertices[v].z);
                boundsMax[0] = std::max(boundsMax[0], m->mVertices[v].x);
                boundsMax[1] = std::max(boundsMax[1], m->mVertices[v].y);
                boundsMax[2] = std::max(boundsMax[2], m->mVertices[v].z);
            }
        }

        // 复制法线（根据 attributeMask 而不是源数据）
        if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::Normal))) {
            for (unsigned int v = 0; v < m->mNumVertices; v++) {
                mesh.normals[(vertexOffset + v) * 3 + 0] = m->mNormals[v].x;
                mesh.normals[(vertexOffset + v) * 3 + 1] = m->mNormals[v].y;
                mesh.normals[(vertexOffset + v) * 3 + 2] = m->mNormals[v].z;
            }
        }

        // 复制 UV
        if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::UV))) {
            for (unsigned int v = 0; v < m->mNumVertices; v++) {
                mesh.uvs[(vertexOffset + v) * 2 + 0] = m->mTextureCoords[0][v].x;
                mesh.uvs[(vertexOffset + v) * 2 + 1] = m->mTextureCoords[0][v].y;
            }
        }

        // 复制切线
        if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
            for (unsigned int v = 0; v < m->mNumVertices; v++) {
                mesh.tangents[(vertexOffset + v) * 4 + 0] = m->mTangents[v].x;
                mesh.tangents[(vertexOffset + v) * 4 + 1] = m->mTangents[v].y;
                mesh.tangents[(vertexOffset + v) * 4 + 2] = m->mTangents[v].z;
                float handedness = (m->mNormals[v] ^ m->mTangents[v]) * m->mBitangents[v] > 0 ? 1.0f : -1.0f;
                mesh.tangents[(vertexOffset + v) * 4 + 3] = handedness;
            }
        }

        // 复制颜色
        if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::Color))) {
            for (unsigned int v = 0; v < m->mNumVertices; v++) {
                mesh.colors[(vertexOffset + v) * 4 + 0] = m->mColors[0][v].r;
                mesh.colors[(vertexOffset + v) * 4 + 1] = m->mColors[0][v].g;
                mesh.colors[(vertexOffset + v) * 4 + 2] = m->mColors[0][v].b;
                mesh.colors[(vertexOffset + v) * 4 + 3] = m->mColors[0][v].a;
            }
        }

        // 复制骨骼权重数据
        if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::SkinWeight))) {
            // 构建骨骼名称到局部索引的映射
            std::unordered_map<std::string, UInt32> boneNameToLocalIndex;
            for (UInt32 bi = 0; bi < m->mNumBones; bi++) {
                boneNameToLocalIndex[m->mBones[bi]->mName.C_Str()] = bi;
            }

            // 遍历每个骨骼，填充顶点权重
            for (UInt32 bi = 0; bi < m->mNumBones; bi++) {
                const aiBone* bone = m->mBones[bi];
                for (unsigned int wi = 0; wi < bone->mNumWeights; wi++) {
                    const aiVertexWeight& vw = bone->mWeights[wi];
                    UInt32 vertexIndex = vertexOffset + vw.mVertexId;
                    UInt32 slot = 0;

                    // 找到第一个空的权重槽
                    for (UInt32 s = 0; s < 4; s++) {
                        if (mesh.skinWeights[vertexIndex * 8 + 4 + s] < 0.001f) {
                            slot = s;
                            break;
                        }
                    }

                    // 填充骨骼索引和权重
                    mesh.skinWeights[vertexIndex * 8 + slot] = static_cast<float>(bi);  // bone index (存储为float)
                    mesh.skinWeights[vertexIndex * 8 + 4 + slot] = vw.mWeight;           // weight
                }
            }

            // 归一化权重（确保总和为1）
            for (UInt32 v = 0; v < meshVertexCount; v++) {
                float totalWeight = 0.0f;
                for (UInt32 s = 0; s < 4; s++) {
                    totalWeight += mesh.skinWeights[(vertexOffset + v) * 8 + 4 + s];
                }
                if (totalWeight > 0.001f) {
                    for (UInt32 s = 0; s < 4; s++) {
                        mesh.skinWeights[(vertexOffset + v) * 8 + 4 + s] /= totalWeight;
                    }
                }
            }
        }

        // 复制索引（需要重新映射顶点索引）
        UInt32 meshIndexCount = 0;
        for (unsigned int f = 0; f < m->mNumFaces; f++) {
            for (unsigned int j = 0; j < m->mFaces[f].mNumIndices; j++) {
                mesh.indices[indexOffset + meshIndexCount++] = vertexOffset + m->mFaces[f].mIndices[j];
            }
        }

        // 添加 submesh
        SubmeshData submesh;
        submesh.startIndex = indexOffset;
        submesh.indexCount = meshIndexCount;
        submesh.vertexOffset = vertexOffset;
        mesh.submeshes.push_back(submesh);

        // Material slot — must match MaterialConverter / FBXConverter contract.
        const std::string base = _assetBaseName.empty() ? "asset" : _assetBaseName;
        mesh.materialSlots.push_back(
            makeMaterialVirtualPath(base, static_cast<std::size_t>(m->mMaterialIndex)));

        vertexOffset += meshVertexCount;
        indexOffset += meshIndexCount;
    }

    // 设置 bounds
    if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::Position))) {
        mesh.boundsMin[0] = boundsMin[0]; mesh.boundsMin[1] = boundsMin[1]; mesh.boundsMin[2] = boundsMin[2];
        mesh.boundsMax[0] = boundsMax[0]; mesh.boundsMax[1] = boundsMax[1]; mesh.boundsMax[2] = boundsMax[2];
    }

    _result->meshes.push_back(std::move(mesh));
}

// 分离模式：每个 FBX 节点一个 MeshData
void FBXParser::_collectNodeMeshes(const aiNode* node, const aiScene* scene, const std::string& parentPath) {
    if (!node) return;

    std::string nodeName = node->mName.C_Str();
    if (nodeName.empty()) {
        nodeName = "unnamed";
    }
    std::string currentPath = parentPath.empty() ? nodeName : parentPath + "/" + nodeName;

    // 如果此节点有 mesh，创建一个 MeshData
    if (node->mNumMeshes > 0) {
        MeshData mesh;
        mesh.name = currentPath;

        UInt32 vertexOffset = 0;
        UInt32 indexOffset = 0;
        UInt8 mergedAttributeMask = 0;

        // 计算总大小
        UInt32 totalVertexCount = 0;
        UInt32 totalIndexCount = 0;
        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            size_t meshIndex = node->mMeshes[i];
            const aiMesh* m = scene->mMeshes[meshIndex];
            if (m) {
                mergedAttributeMask |= _getMeshAttributeMask(m);
                totalVertexCount += m->mNumVertices;
                for (unsigned int f = 0; f < m->mNumFaces; f++) {
                    totalIndexCount += m->mFaces[f].mNumIndices;
                }
            }
        }

        // MeshOnly 模式：只保留 Position 和 UV，忽略 Normal/Tangent/Color
        if (_loadOption == IConverter::LoadOption::MeshOnly) {
            mergedAttributeMask &= (1u << static_cast<UInt8>(MeshAttribute::Position)) |
                                   (1u << static_cast<UInt8>(MeshAttribute::UV));
        }

        mesh.attributeMask = mergedAttributeMask;

        // 分配内存
        mesh.positions.resize(totalVertexCount * 3);
        mesh.indices.resize(totalIndexCount);
        if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::Normal))) {
            mesh.normals.resize(totalVertexCount * 3);
        }
        if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::UV))) {
            mesh.uvs.resize(totalVertexCount * 2);
        }
        if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
            mesh.tangents.resize(totalVertexCount * 4);
        }
        if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::Color))) {
            mesh.colors.resize(totalVertexCount * 4);
        }
        if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::SkinWeight))) {
            mesh.skinWeights.resize(totalVertexCount * 8, 0.0f);
            for (UInt32 v = 0; v < totalVertexCount; v++) {
                mesh.skinWeights[v * 8 + 0] = 0.0f;
                mesh.skinWeights[v * 8 + 1] = 0.0f;
                mesh.skinWeights[v * 8 + 2] = 0.0f;
                mesh.skinWeights[v * 8 + 3] = 0.0f;
                mesh.skinWeights[v * 8 + 4] = 1.0f;
                mesh.skinWeights[v * 8 + 5] = 0.0f;
                mesh.skinWeights[v * 8 + 6] = 0.0f;
                mesh.skinWeights[v * 8 + 7] = 0.0f;
            }
        }

        float boundsMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float boundsMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

        // 复制每个 aiMesh 的数据
        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            size_t meshIndex = node->mMeshes[i];
            const aiMesh* m = scene->mMeshes[meshIndex];
            if (!m) continue;

            UInt32 meshVertexCount = m->mNumVertices;

            // 复制顶点位置
            if (m->HasPositions()) {
                for (unsigned int v = 0; v < m->mNumVertices; v++) {
                    mesh.positions[(vertexOffset + v) * 3 + 0] = m->mVertices[v].x;
                    mesh.positions[(vertexOffset + v) * 3 + 1] = m->mVertices[v].y;
                    mesh.positions[(vertexOffset + v) * 3 + 2] = m->mVertices[v].z;

                    boundsMin[0] = std::min(boundsMin[0], m->mVertices[v].x);
                    boundsMin[1] = std::min(boundsMin[1], m->mVertices[v].y);
                    boundsMin[2] = std::min(boundsMin[2], m->mVertices[v].z);
                    boundsMax[0] = std::max(boundsMax[0], m->mVertices[v].x);
                    boundsMax[1] = std::max(boundsMax[1], m->mVertices[v].y);
                    boundsMax[2] = std::max(boundsMax[2], m->mVertices[v].z);
                }
            }

            // 复制法线（根据 attributeMask 而不是源数据）
            if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::Normal))) {
                for (unsigned int v = 0; v < m->mNumVertices; v++) {
                    mesh.normals[(vertexOffset + v) * 3 + 0] = m->mNormals[v].x;
                    mesh.normals[(vertexOffset + v) * 3 + 1] = m->mNormals[v].y;
                    mesh.normals[(vertexOffset + v) * 3 + 2] = m->mNormals[v].z;
                }
            }

            // 复制 UV
            if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::UV))) {
                for (unsigned int v = 0; v < m->mNumVertices; v++) {
                    mesh.uvs[(vertexOffset + v) * 2 + 0] = m->mTextureCoords[0][v].x;
                    mesh.uvs[(vertexOffset + v) * 2 + 1] = m->mTextureCoords[0][v].y;
                }
            }

            // 复制切线
            if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
                for (unsigned int v = 0; v < m->mNumVertices; v++) {
                    mesh.tangents[(vertexOffset + v) * 4 + 0] = m->mTangents[v].x;
                    mesh.tangents[(vertexOffset + v) * 4 + 1] = m->mTangents[v].y;
                    mesh.tangents[(vertexOffset + v) * 4 + 2] = m->mTangents[v].z;
                    float handedness = (m->mNormals[v] ^ m->mTangents[v]) * m->mBitangents[v] > 0 ? 1.0f : -1.0f;
                    mesh.tangents[(vertexOffset + v) * 4 + 3] = handedness;
                }
            }

            // 复制颜色
            if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::Color))) {
                for (unsigned int v = 0; v < m->mNumVertices; v++) {
                    mesh.colors[(vertexOffset + v) * 4 + 0] = m->mColors[0][v].r;
                    mesh.colors[(vertexOffset + v) * 4 + 1] = m->mColors[0][v].g;
                    mesh.colors[(vertexOffset + v) * 4 + 2] = m->mColors[0][v].b;
                    mesh.colors[(vertexOffset + v) * 4 + 3] = m->mColors[0][v].a;
                }
            }

            // 复制骨骼权重数据
            if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::SkinWeight))) {
                // 构建骨骼名称到局部索引的映射
                std::unordered_map<std::string, UInt32> boneNameToLocalIndex;
                for (UInt32 bi = 0; bi < m->mNumBones; bi++) {
                    boneNameToLocalIndex[m->mBones[bi]->mName.C_Str()] = bi;
                }

                // 遍历每个骨骼，填充顶点权重
                for (UInt32 bi = 0; bi < m->mNumBones; bi++) {
                    const aiBone* bone = m->mBones[bi];
                    for (unsigned int wi = 0; wi < bone->mNumWeights; wi++) {
                        const aiVertexWeight& vw = bone->mWeights[wi];
                        UInt32 vertexIndex = vertexOffset + vw.mVertexId;
                        UInt32 slot = 0;
                        for (UInt32 s = 0; s < 4; s++) {
                            if (mesh.skinWeights[vertexIndex * 8 + 4 + s] < 0.001f) {
                                slot = s;
                                break;
                            }
                        }
                        mesh.skinWeights[vertexIndex * 8 + slot] = static_cast<float>(bi);
                        mesh.skinWeights[vertexIndex * 8 + 4 + slot] = vw.mWeight;
                    }
                }

                // 归一化权重
                for (UInt32 v = 0; v < meshVertexCount; v++) {
                    float totalWeight = 0.0f;
                    for (UInt32 s = 0; s < 4; s++) {
                        totalWeight += mesh.skinWeights[(vertexOffset + v) * 8 + 4 + s];
                    }
                    if (totalWeight > 0.001f) {
                        for (UInt32 s = 0; s < 4; s++) {
                            mesh.skinWeights[(vertexOffset + v) * 8 + 4 + s] /= totalWeight;
                        }
                    }
                }
            }

            // 复制索引
            UInt32 meshIndexCount = 0;
            for (unsigned int f = 0; f < m->mNumFaces; f++) {
                for (unsigned int j = 0; j < m->mFaces[f].mNumIndices; j++) {
                    mesh.indices[indexOffset + meshIndexCount++] = vertexOffset + m->mFaces[f].mIndices[j];
                }
            }

            // 添加 submesh
            SubmeshData submesh;
            submesh.startIndex = indexOffset;
            submesh.indexCount = meshIndexCount;
            submesh.vertexOffset = vertexOffset;
            mesh.submeshes.push_back(submesh);

            // Material slot — must match MaterialConverter / FBXConverter contract.
            const std::string base = _assetBaseName.empty() ? "asset" : _assetBaseName;
            mesh.materialSlots.push_back(
                makeMaterialVirtualPath(base, static_cast<std::size_t>(m->mMaterialIndex)));

            vertexOffset += meshVertexCount;
            indexOffset += meshIndexCount;
        }

        // 设置 bounds
        if (mergedAttributeMask & (1u << static_cast<UInt8>(MeshAttribute::Position))) {
            mesh.boundsMin[0] = boundsMin[0]; mesh.boundsMin[1] = boundsMin[1]; mesh.boundsMin[2] = boundsMin[2];
            mesh.boundsMax[0] = boundsMax[0]; mesh.boundsMax[1] = boundsMax[1]; mesh.boundsMax[2] = boundsMax[2];
        }

        _result->meshes.push_back(std::move(mesh));
    }

    // 递归处理子节点
    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        _collectNodeMeshes(node->mChildren[i], scene, currentPath);
    }
}

// 提取材质纹理路径
void FBXParser::_extractMaterialTextures(const aiMaterial* mat, MaterialData& material) {
    // 纹理类型映射到参数名
    static const std::pair<aiTextureType, const char*> textureTypes[] = {
        {aiTextureType_DIFFUSE, "baseColorTexture"},
        {aiTextureType_NORMALS, "normalTexture"},
        {aiTextureType_SPECULAR, "specularTexture"},
        {aiTextureType_EMISSIVE, "emissiveTexture"},
        {aiTextureType_HEIGHT, "heightTexture"},
        {aiTextureType_OPACITY, "opacityTexture"},
        {aiTextureType_BASE_COLOR, "baseColorTexture"},  // PBR base color
        {aiTextureType_NORMAL_CAMERA, "normalCameraTexture"},
        {aiTextureType_EMISSION_COLOR, "emissionColorTexture"},
        {aiTextureType_METALNESS, "metallicTexture"},
        {aiTextureType_DIFFUSE_ROUGHNESS, "roughnessTexture"},
        {aiTextureType_AMBIENT_OCCLUSION, "aoTexture"},
        {aiTextureType_REFLECTION, "reflectionTexture"},
        {aiTextureType_UNKNOWN, "unknownTexture"},
    };

    for (const auto& [texType, paramName] : textureTypes) {
        // 检查是否有该类型的纹理
        aiTextureType mappedType = texType;
        unsigned int texCount = mat->GetTextureCount(mappedType);
        if (texCount == 0) continue;

        // 获取第一个纹理路径
        aiString texPath;
        if (mat->GetTexture(mappedType, 0, &texPath) == AI_SUCCESS) {
            std::string path(texPath.C_Str());
            if (path.empty()) continue;

            // Virtual path uses flattened stem + usage + .aytex.
            // texturePaths keeps the Assimp path so convertFromPath can
            // open absolute or FBX-relative sources.
            const std::string textureName = makeTextureStemFromSourcePath(path);

            Param param;
            param.name = paramName;
            param.type = MaterialParamType::Texture2D;
            param.texturePath = makeTextureVirtualPath(textureName, _textureUsageSuffix);
            material.parameters.push_back(param);

            material.texturePaths.push_back(path);
        }
    }
}

void FBXParser::_parseMaterial(const void* aiMatPtr, size_t index) {
    const aiMaterial* mat = static_cast<const aiMaterial*>(aiMatPtr);

    MaterialData material;
    material.name = "material_" + std::to_string(index);
    // Editor ships simple_lit_shadow.phoskia under the asset root.
    // pbr.phoskia is not generated by Import — using it made loadMaterial
    // fail and SkinnedMesh fell back to an untextured white GBuffer fill.
    material.shader = "simple_lit_shadow.phoskia";

    // baseColor (albedo)
    aiColor4D baseColor;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS) {
        Param param;
        param.name = "baseColor";
        param.type = MaterialParamType::Float4;
        param.float4Value[0] = baseColor.r;
        param.float4Value[1] = baseColor.g;
        param.float4Value[2] = baseColor.b;
        param.float4Value[3] = baseColor.a;
        material.parameters.push_back(param);
    }

    // metallic (PBR) - 检查常见建模引擎导出的字符串属性
    aiString metallicStr;
    if (mat->Get("$mat.pbrMetallicFactor", 0, 0, metallicStr) == AI_SUCCESS ||
        mat->Get("metallic", 0, 0, metallicStr) == AI_SUCCESS ||
        mat->Get("Metallic", 0, 0, metallicStr) == AI_SUCCESS ||
        mat->Get("$mat.glmModelPBR.metallic", 0, 0, metallicStr) == AI_SUCCESS) {
        try {
            float metallic = std::stof(metallicStr.C_Str());
            if (metallic >= 0.0f && metallic <= 1.0f) {
                Param param;
                param.name = "metallic";
                param.type = MaterialParamType::Float;
                param.floatValue = metallic;
                material.parameters.push_back(param);
            }
        } catch (...) {}
    }

    // roughness (PBR) - 检查常见建模引擎导出的字符串属性
    aiString roughnessStr;
    if (mat->Get("$mat.pbrRoughnessFactor", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("roughness", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("Roughness", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("$mat.glmModelPBR.roughness", 0, 0, roughnessStr) == AI_SUCCESS) {
        try {
            float roughness = std::stof(roughnessStr.C_Str());
            if (roughness >= 0.0f && roughness <= 1.0f) {
                Param param;
                param.name = "roughness";
                param.type = MaterialParamType::Float;
                param.floatValue = roughness;
                material.parameters.push_back(param);
            }
        } catch (...) {}
    }

    // emissive
    aiColor4D emissive;
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
        Param param;
        param.name = "emissive";
        param.type = MaterialParamType::Float3;
        param.float3Value[0] = emissive.r;
        param.float3Value[1] = emissive.g;
        param.float3Value[2] = emissive.b;
        material.parameters.push_back(param);
    }

    // opacity (transparency)
    float opacity = 1.0f;
    if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
        Param param;
        param.name = "opacity";
        param.type = MaterialParamType::Float;
        param.floatValue = opacity;
        material.parameters.push_back(param);
    }

    // 提取纹理路径
    _extractMaterialTextures(mat, material);

    // specular
    aiColor4D specular;
    if (mat->Get(AI_MATKEY_COLOR_SPECULAR, specular) == AI_SUCCESS) {
        Param param;
        param.name = "specular";
        param.type = MaterialParamType::Float4;
        param.float4Value[0] = specular.r;
        param.float4Value[1] = specular.g;
        param.float4Value[2] = specular.b;
        param.float4Value[3] = specular.a;
        material.parameters.push_back(param);
    }

    // shininess (specular power)
    float shininess = 0.0f;
    if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS) {
        Param param;
        param.name = "shininess";
        param.type = MaterialParamType::Float;
        param.floatValue = shininess;
        material.parameters.push_back(param);
    }

    _result->materials.push_back(std::move(material));
}

void FBXParser::_parseTexture(const void* aiTexPtr, size_t index) {
    const aiTexture* tex = static_cast<const aiTexture*>(aiTexPtr);

    TextureData texture;
    texture.name = "texture_" + std::to_string(index);

    if (tex->mWidth > 0) {
        texture.width = tex->mWidth;
        texture.height = tex->mHeight;
        texture.format = TextureFormat::RGBA8;
        texture.imageData.resize(tex->mWidth * tex->mHeight * 4);
        memcpy(texture.imageData.data(), tex->pcData, texture.imageData.size());
    }

    _result->textures.push_back(std::move(texture));
}

UInt8 FBXParser::_getMeshAttributeMask(const aiMesh* m) {
    UInt8 mask = 0;
    if (m->HasPositions()) mask |= (1u << static_cast<UInt8>(MeshAttribute::Position));
    if (m->HasNormals()) mask |= (1u << static_cast<UInt8>(MeshAttribute::Normal));
    if (m->HasTextureCoords(0)) mask |= (1u << static_cast<UInt8>(MeshAttribute::UV));
    if (m->HasTangentsAndBitangents()) mask |= (1u << static_cast<UInt8>(MeshAttribute::Tangent));
    if (m->HasVertexColors(0)) mask |= (1u << static_cast<UInt8>(MeshAttribute::Color));
    if (m->HasBones()) mask |= (1u << static_cast<UInt8>(MeshAttribute::SkinWeight));
    return mask;
}

void FBXParser::_parseSkeletons(const aiScene* scene) {
    // 收集所有骨骼节点名称（在任意 mesh 中出现的骨骼）
    std::unordered_set<std::string> boneNodeNames;
    for (unsigned int mi = 0; mi < scene->mNumMeshes; mi++) {
        const aiMesh* m = scene->mMeshes[mi];
        for (unsigned int bi = 0; bi < m->mNumBones; bi++) {
            boneNodeNames.insert(std::string(m->mBones[bi]->mName.C_Str()));
        }
    }

    if (boneNodeNames.empty()) {
        return; // 没有骨骼
    }

    // 创建 SkeletonData 并收集骨骼
    SkeletonData skeleton;
    skeleton.name = "Skeleton";
    _collectSkeletonBones(scene->mRootNode, -1, boneNodeNames, skeleton);

    if (!skeleton.bones.empty()) {
        _result->skeletons.push_back(std::move(skeleton));
    }
}

void FBXParser::_collectSkeletonBones(const aiNode* node, int parentIndex,
                                      const std::unordered_set<std::string>& boneNodeNames,
                                      SkeletonData& skeleton) {
    if (!node) return;

    std::string nodeName = node->mName.C_Str();

    // 检查是否是骨骼节点
    bool isBone = boneNodeNames.find(nodeName) != boneNodeNames.end();

    int thisIndex = -1;
    if (isBone) {
        BoneData bone;
        bone.name = nodeName;
        bone.parentIndex = parentIndex;

        // 获取节点变换作为绑定姿势的逆矩阵
        aiMatrix4x4 invBind = node->mTransformation;
        invBind.Inverse();
        // 转换为 Float4x4 (Assimp 是 column-major，AYMath 也是 column-major)
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                bone.inverseBindMatrix(r, c) = invBind[r][c];
            }
        }

        // R-02: 从 aiNode::mTransformation 分解 TRS,作为本地 rest pose
        // Assimp 的 Decompose 返回 void,对正常 TRS 输入总是写入值;
        // 极端退化情况(如全 0 缩放)由调用方在后续步过滤,这里直接采信。
        aiVector3D trans;
        aiQuaternion rot;
        aiVector3D scale;
        node->mTransformation.Decompose(scale, rot, trans);
        bone.localPosition = ayt::math::FVector3(trans.x, trans.y, trans.z);
        bone.localRotation = ayt::math::FQuaternion(rot.x, rot.y, rot.z, rot.w);
        bone.localScale    = ayt::math::FVector3(scale.x, scale.y, scale.z);

        thisIndex = static_cast<int>(skeleton.bones.size());
        skeleton.bones.push_back(std::move(bone));
    }

    // 递归处理子节点
    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        _collectSkeletonBones(node->mChildren[i], thisIndex, boneNodeNames, skeleton);
    }
}

// R-02: scene->mAnimations → IntermediateAsset::animations
// 每个 aiAnimation = 一个 take,转换为一条 AnimationData。
// 每个 aiNodeAnim channel 对应一个骨骼;按 position/rotation/scale 拆为 3 条 KeyframeTrack
// (空 track 跳过;valueType 按 property 推断)。
//
// Phase 1.5 TODO: notify markers. FBX has no first-class notify channel.
// assimp exposes `mAnim->mName` and (via assimp_metadata.h) generic
// key/value bags on nodes, but no standardized "event" or "curve node"
// type. Two paths to extend this function when notify authoring becomes
// a requirement:
//   (a) scan `scene->mRootNode->mMetaData` / per-channel `mNodeAnim->mMetaData`
//       for keys like "OnFootstep" / "OnHit" and read their `time` + `payload`
//       from sibling entries.
//   (b) accept a sibling `.notifies.json` file at conversion time and merge
//       into `data.notifies` after the track loop below.
// For the first cut we leave `data.notifies` empty; AYAnimation handles the
// zero-marker case trivially (`getNotifyCount() == 0`, no dispatch overhead).
void FBXParser::_parseAnimations(const aiScene* scene) {
    if (!scene) return;
    if (scene->mNumAnimations == 0) return;

    for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* anim = scene->mAnimations[ai];
        if (!anim) continue;

        AnimationData data;
        data.name = std::string(anim->mName.C_Str());
        data.duration = static_cast<Float32>(anim->mDuration);
        // mTicksPerSecond == 0 在 Assimp 契约里表示 "use scene default",fallback 30
        data.ticksPerSecond = anim->mTicksPerSecond != 0.0
            ? static_cast<Float32>(anim->mTicksPerSecond)
            : 30.0f;

        const double ticks = (anim->mTicksPerSecond != 0.0)
            ? anim->mTicksPerSecond
            : 30.0;

        for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci) {
            const aiNodeAnim* chan = anim->mChannels[ci];
            if (!chan) continue;

            const std::string nodeName = chan->mNodeName.C_Str();

            // ---- Position track (Vector3) ----
            if (chan->mNumPositionKeys > 0) {
                KeyframeTrack tr;
                tr.targetNode = nodeName;
                tr.property = "position";
                tr.valueType = AnimTrackType::Vector3;
                tr.times.reserve(chan->mNumPositionKeys);
                tr.values.reserve(chan->mNumPositionKeys * 3);
                for (unsigned int k = 0; k < chan->mNumPositionKeys; ++k) {
                    tr.times.push_back(static_cast<Float32>(chan->mPositionKeys[k].mTime / ticks));
                    tr.values.push_back(chan->mPositionKeys[k].mValue.x);
                    tr.values.push_back(chan->mPositionKeys[k].mValue.y);
                    tr.values.push_back(chan->mPositionKeys[k].mValue.z);
                }
                data.tracks.push_back(std::move(tr));
            }

            // ---- Rotation track (Quaternion) ----
            if (chan->mNumRotationKeys > 0) {
                KeyframeTrack tr;
                tr.targetNode = nodeName;
                tr.property = "rotation";
                tr.valueType = AnimTrackType::Quaternion;
                tr.times.reserve(chan->mNumRotationKeys);
                tr.values.reserve(chan->mNumRotationKeys * 4);
                for (unsigned int k = 0; k < chan->mNumRotationKeys; ++k) {
                    tr.times.push_back(static_cast<Float32>(chan->mRotationKeys[k].mTime / ticks));
                    // assimp quat: (x, y, z, w); 我们 IAnimation 期望 (x, y, z, w) 顺序,直接 memcpy
                    tr.values.push_back(chan->mRotationKeys[k].mValue.x);
                    tr.values.push_back(chan->mRotationKeys[k].mValue.y);
                    tr.values.push_back(chan->mRotationKeys[k].mValue.z);
                    tr.values.push_back(chan->mRotationKeys[k].mValue.w);
                }
                data.tracks.push_back(std::move(tr));
            }

            // ---- Scale track (Vector3) ----
            if (chan->mNumScalingKeys > 0) {
                KeyframeTrack tr;
                tr.targetNode = nodeName;
                tr.property = "scale";
                tr.valueType = AnimTrackType::Vector3;
                tr.times.reserve(chan->mNumScalingKeys);
                tr.values.reserve(chan->mNumScalingKeys * 3);
                for (unsigned int k = 0; k < chan->mNumScalingKeys; ++k) {
                    tr.times.push_back(static_cast<Float32>(chan->mScalingKeys[k].mTime / ticks));
                    tr.values.push_back(chan->mScalingKeys[k].mValue.x);
                    tr.values.push_back(chan->mScalingKeys[k].mValue.y);
                    tr.values.push_back(chan->mScalingKeys[k].mValue.z);
                }
                data.tracks.push_back(std::move(tr));
            }
        }

        // 空 take (零轨道) 不加入 — 让 AnimationConverter 走"无 anim"分支
        if (!data.tracks.empty()) {
            _result->animations.push_back(std::move(data));
        }
    }
}

} // namespace ayt::resource
