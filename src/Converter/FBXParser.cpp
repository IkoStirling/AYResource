#include "AYResource/Converter/FBXParser.h"
#include "AYResource/VirtualAssetPath.h"
#include "AYResource/assetsDefs/IMesh.h"
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <functional>

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
    // FBXImporter already interprets the file's declared Up axis unless this
    // property is explicitly disabled. Convert source units to meters before
    // post-processing so meshes, node translations, bone offsets and
    // animation translations all share the same scale.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION, false);
    importer.SetPropertyBool(AI_CONFIG_FBX_CONVERT_TO_M, true);

    unsigned int flags = aiProcess_Triangulate
                       | aiProcess_JoinIdenticalVertices
                       | aiProcess_MakeLeftHanded
                       | aiProcess_FlipWindingOrder
                       | aiProcess_LimitBoneWeights;
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
    _prepareSkeletonMapping(scene);

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
    if (!_validateSkinningContract()) {
        return false;
    }

    // R-02: 解析 Animations (MeshOnly 跳过;Full 时全量提取)
    if (_loadOption != IConverter::LoadOption::MeshOnly) {
        _parseAnimations(scene);
    }

    return !_result->meshes.empty();
}

std::unique_ptr<IntermediateAsset> FBXParser::getResult() {
    return std::move(_result);
}

namespace {

ayt::math::Float4x4 toAyMatrix(const aiMatrix4x4& source)
{
    ayt::math::Float4x4 out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out(r, c) = source[r][c];
        }
    }
    return out;
}

} // namespace

void FBXParser::_prepareSkeletonMapping(const aiScene* scene)
{
    _boneNameToIndex.clear();
    _boneOffsets.clear();
    _boneNodeNames.clear();
    if (!scene) return;

    // aiBone::mOffsetMatrix is the authoritative mesh-bind -> bone-bind
    // transform. Keep the first value for duplicate bone names; FBX meshes in
    // one armature should expose the same offset for every occurrence.
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh) continue;
        for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi) {
            const aiBone* bone = mesh->mBones[bi];
            if (!bone) continue;
            const std::string name = bone->mName.C_Str();
            _boneNodeNames.insert(name);
            _boneOffsets.emplace(name, toAyMatrix(bone->mOffsetMatrix));
        }
    }

    // Runtime palettes require parent-before-child ordering. Assign the
    // scene-wide indices by hierarchy, never by per-mesh mBones order.
    std::function<void(const aiNode*)> visit = [&](const aiNode* node) {
        if (!node) return;
        const std::string name = node->mName.C_Str();
        if (_boneNodeNames.find(name) != _boneNodeNames.end()
            && _boneNameToIndex.find(name) == _boneNameToIndex.end()) {
            _boneNameToIndex.emplace(
                name, static_cast<UInt32>(_boneNameToIndex.size()));
        }
        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            visit(node->mChildren[i]);
        }
    };
    visit(scene->mRootNode);

    // Malformed files can reference a bone with no matching aiNode. Preserve
    // a deterministic slot so vertex weights never silently target bone 0;
    // _parseSkeletons emits an identity-rest fallback for these slots.
    std::vector<std::string> missingNames;
    for (const std::string& name : _boneNodeNames) {
        if (_boneNameToIndex.find(name) == _boneNameToIndex.end()) {
            missingNames.push_back(name);
        }
    }
    std::sort(missingNames.begin(), missingNames.end());
    for (const std::string& name : missingNames) {
            _boneNameToIndex.emplace(
                name, static_cast<UInt32>(_boneNameToIndex.size()));
    }
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
        // Start empty. A fallback to bone 0 is installed only after all real
        // influences have been collected; pre-seeding weight0=1 blended every
        // skinned vertex with bone 0 and visibly distorted multi-part models.
        for (UInt32 v = 0; v < totalVertexCount; v++) {
            mesh.skinWeights[v * 8 + 0] = 0.0f;  // bone index 0
            mesh.skinWeights[v * 8 + 1] = 0.0f;  // bone index 1
            mesh.skinWeights[v * 8 + 2] = 0.0f;  // bone index 2
            mesh.skinWeights[v * 8 + 3] = 0.0f;  // bone index 3
            mesh.skinWeights[v * 8 + 4] = 0.0f;
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
            // 遍历每个骨骼，填充顶点权重
            for (UInt32 bi = 0; bi < m->mNumBones; bi++) {
                const aiBone* bone = m->mBones[bi];
                const auto globalBone =
                    _boneNameToIndex.find(bone->mName.C_Str());
                if (globalBone == _boneNameToIndex.end()) continue;
                for (unsigned int wi = 0; wi < bone->mNumWeights; wi++) {
                    const aiVertexWeight& vw = bone->mWeights[wi];
                    UInt32 vertexIndex = vertexOffset + vw.mVertexId;
                    UInt32 slot = 4;

                    // 找到第一个空的权重槽
                    for (UInt32 s = 0; s < 4; s++) {
                        if (mesh.skinWeights[vertexIndex * 8 + 4 + s] < 0.001f) {
                            slot = s;
                            break;
                        }
                    }

                    if (slot == 4) continue;
                    // Store the scene-wide SkeletonData palette index.
                    mesh.skinWeights[vertexIndex * 8 + slot] =
                        static_cast<float>(globalBone->second);
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
                } else {
                    mesh.skinWeights[(vertexOffset + v) * 8 + 0] = 0.0f;
                    mesh.skinWeights[(vertexOffset + v) * 8 + 4] = 1.0f;
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
        // materialSlots is emitted in the same order as submeshes.  The
        // runtime index must address that vector, not Assimp's scene-wide
        // material table (which can be sparse after mesh filtering).
        submesh.materialIndex = static_cast<UInt32>(mesh.materialSlots.size());
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
                mesh.skinWeights[v * 8 + 4] = 0.0f;
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
                // 遍历每个骨骼，填充顶点权重
                for (UInt32 bi = 0; bi < m->mNumBones; bi++) {
                    const aiBone* bone = m->mBones[bi];
                    const auto globalBone =
                        _boneNameToIndex.find(bone->mName.C_Str());
                    if (globalBone == _boneNameToIndex.end()) continue;
                    for (unsigned int wi = 0; wi < bone->mNumWeights; wi++) {
                        const aiVertexWeight& vw = bone->mWeights[wi];
                        UInt32 vertexIndex = vertexOffset + vw.mVertexId;
                        UInt32 slot = 4;
                        for (UInt32 s = 0; s < 4; s++) {
                            if (mesh.skinWeights[vertexIndex * 8 + 4 + s] < 0.001f) {
                                slot = s;
                                break;
                            }
                        }
                        if (slot == 4) continue;
                        mesh.skinWeights[vertexIndex * 8 + slot] =
                            static_cast<float>(globalBone->second);
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
                    } else {
                        mesh.skinWeights[(vertexOffset + v) * 8 + 0] = 0.0f;
                        mesh.skinWeights[(vertexOffset + v) * 8 + 4] = 1.0f;
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
            // Keep the submesh-to-slot mapping explicit.  Leaving the
            // default zero here made every part of a merged character use
            // the first material at runtime.
            submesh.materialIndex = static_cast<UInt32>(mesh.materialSlots.size());
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

            // Virtual path uses flattened stem + usage + extension.
            // texturePaths keeps the Assimp path so convertFromPath can
            // open absolute or FBX-relative sources.
            const std::string textureName = makeTextureStemFromSourcePath(path);

            // Dev raw-reference mode: keep the source extension (e.g. .png)
            // so .aymat points at the raw image copied into textures/;
            // TextureLoader decodes it with stb at runtime. dds/aytex
            // sources stay on .aytex (zero-decode passthrough cook, and
            // .dds is not a registered runtime extension).
            const std::string texExt =
                _preserveSourceExtension ? textureDevExtensionOf(path) : ".aytex";

            Param param;
            param.name = paramName;
            param.type = MaterialParamType::Texture2D;
            param.texturePath = makeTextureVirtualPath(textureName, _textureUsageSuffix,
                                                       texExt.c_str());
            material.parameters.push_back(param);

            material.texturePaths.push_back(path);
        }
    }
}

void FBXParser::_parseMaterial(const void* aiMatPtr, size_t index) {
    const aiMaterial* mat = static_cast<const aiMaterial*>(aiMatPtr);

    MaterialData material;
    aiString sourceMaterialName;
    if (mat->Get(AI_MATKEY_NAME, sourceMaterialName) == AI_SUCCESS
        && sourceMaterialName.length > 0) {
        material.name = sourceMaterialName.C_Str();
    } else {
        material.name = "material_" + std::to_string(index);
    }
    // The runtime PBR source is shipped by AYRenderer and seeded into the
    // Editor asset root. Keep this flat virtual path aligned with
    // RenderAssetBridge's root-relative shader resolution.
    material.shader = "pbr.phoskia";

    // Import surface metadata into the typed material contract. FBX files
    // that expose a real scalar opacity or blend state get Blend; otherwise
    // stay conservatively Opaque. An opacity texture alone is deliberately
    // not enough: many FBX exporters connect the base-color texture to the
    // TransparencyFactor slot for every material.
    float importedOpacity = 1.0f;
    const bool hasOpacity =
        mat->Get(AI_MATKEY_OPACITY, importedOpacity) == AI_SUCCESS;
    aiBlendMode importedBlend = aiBlendMode_Default;
    const bool hasBlend =
        mat->Get(AI_MATKEY_BLEND_FUNC, importedBlend) == AI_SUCCESS;
    int importedTwoSided = 0;
    (void)mat->Get(AI_MATKEY_TWOSIDED, importedTwoSided);

    const bool explicitBlend = (hasOpacity && importedOpacity < 0.999f) || hasBlend;
    material.alphaMode = explicitBlend
        ? MaterialAlphaMode::Blend : MaterialAlphaMode::Opaque;
    material.alphaCutoff = 0.5f;
    material.doubleSided = importedTwoSided != 0;
    material.surfaceSource = (explicitBlend || material.doubleSided)
        ? MaterialSurfaceSource::ExplicitSource
        : MaterialSurfaceSource::Default;

    // baseColor (albedo)
    aiColor4D baseColor;
    baseColor.r = 1.0f;
    baseColor.g = 1.0f;
    baseColor.b = 1.0f;
    baseColor.a = 1.0f;
    mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor);
    Param baseColorParam;
    baseColorParam.name = "baseColor";
    baseColorParam.type = MaterialParamType::Float4;
    baseColorParam.float4Value[0] = baseColor.r;
    baseColorParam.float4Value[1] = baseColor.g;
    baseColorParam.float4Value[2] = baseColor.b;
    baseColorParam.float4Value[3] = baseColor.a;
    material.parameters.push_back(baseColorParam);

    // metallic (PBR) - 检查常见建模引擎导出的字符串属性
    float metallic = 0.0f;
    aiString metallicStr;
    if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) != AI_SUCCESS &&
        (mat->Get("$mat.pbrMetallicFactor", 0, 0, metallicStr) == AI_SUCCESS ||
        mat->Get("metallic", 0, 0, metallicStr) == AI_SUCCESS ||
        mat->Get("Metallic", 0, 0, metallicStr) == AI_SUCCESS ||
        mat->Get("$mat.glmModelPBR.metallic", 0, 0, metallicStr) == AI_SUCCESS)) {
        try {
            const float parsed = std::stof(metallicStr.C_Str());
            if (parsed >= 0.0f && parsed <= 1.0f) {
                metallic = parsed;
            }
        } catch (...) {}
    }
    Param metallicParam;
    metallicParam.name = "metallic";
    metallicParam.type = MaterialParamType::Float;
    metallicParam.floatValue = metallic;
    material.parameters.push_back(metallicParam);

    // roughness (PBR) - 检查常见建模引擎导出的字符串属性
    float roughness = 0.5f;
    aiString roughnessStr;
    if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) != AI_SUCCESS &&
        (mat->Get("$mat.pbrRoughnessFactor", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("roughness", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("Roughness", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("$mat.glmModelPBR.roughness", 0, 0, roughnessStr) == AI_SUCCESS)) {
        try {
            const float parsed = std::stof(roughnessStr.C_Str());
            if (parsed >= 0.0f && parsed <= 1.0f) {
                roughness = parsed;
            }
        } catch (...) {}
    }
    Param roughnessParam;
    roughnessParam.name = "roughness";
    roughnessParam.type = MaterialParamType::Float;
    roughnessParam.floatValue = roughness;
    material.parameters.push_back(roughnessParam);

    Param aoParam;
    aoParam.name = "ao";
    aoParam.type = MaterialParamType::Float;
    aoParam.floatValue = 1.0f;
    material.parameters.push_back(aoParam);

    // emissive
    aiColor4D emissive;
    emissive.r = 0.0f;
    emissive.g = 0.0f;
    emissive.b = 0.0f;
    emissive.a = 1.0f;
    mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
    Param emissiveParam;
    emissiveParam.name = "emissive";
    emissiveParam.type = MaterialParamType::Float3;
    emissiveParam.float3Value[0] = emissive.r;
    emissiveParam.float3Value[1] = emissive.g;
    emissiveParam.float3Value[2] = emissive.b;
    material.parameters.push_back(emissiveParam);

    // opacity (transparency)
    float opacity = 1.0f;
    mat->Get(AI_MATKEY_OPACITY, opacity);
    Param opacityParam;
    opacityParam.name = "opacity";
    opacityParam.type = MaterialParamType::Float;
    opacityParam.floatValue = opacity;
    material.parameters.push_back(opacityParam);

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
    if (_boneNodeNames.empty()) {
        return; // 没有骨骼
    }

    // Pre-size to the exact scene-wide palette used by mesh weights.
    SkeletonData skeleton;
    skeleton.name = "Skeleton";
    skeleton.bones.resize(_boneNameToIndex.size());
    _collectSkeletonBones(scene->mRootNode, -1, _boneNodeNames, skeleton,
                          ayt::math::Float4x4::identity());

    // A referenced aiBone without a matching node is malformed but can still
    // be represented deterministically. Keep its authoritative offset and an
    // identity rest transform; diagnostics make the degradation visible.
    for (const auto& entry : _boneNameToIndex) {
        BoneData& bone = skeleton.bones[entry.second];
        if (!bone.name.empty()) continue;
        bone.name = entry.first;
        bone.parentIndex = -1;
        const auto offset = _boneOffsets.find(entry.first);
        bone.inverseBindMatrix = offset != _boneOffsets.end()
            ? offset->second : ayt::math::Float4x4::identity();
        std::fprintf(stderr,
                     "[FBXParser] bone '%s' has no matching node; "
                     "using identity rest pose\n",
                     entry.first.c_str());
    }

    if (!skeleton.bones.empty()) {
        _result->skeletons.push_back(std::move(skeleton));
    }
}

bool FBXParser::_validateSkinningContract() const
{
    std::size_t boneCount = 0;
    if (!_result->skeletons.empty()) {
        boneCount = _result->skeletons.front().bones.size();
        for (std::size_t i = 0; i < boneCount; ++i) {
            const BoneData& bone = _result->skeletons.front().bones[i];
            if (bone.name.empty()
                || bone.parentIndex >= static_cast<int>(i)) {
                std::fprintf(stderr,
                             "[FBXParser] invalid skeleton palette at bone %zu "
                             "name='%s' parent=%d\n",
                             i, bone.name.c_str(), bone.parentIndex);
                return false;
            }
        }
    }

    for (const MeshData& mesh : _result->meshes) {
        if (mesh.skinWeights.empty()) continue;
        const std::size_t vertexCount = mesh.positions.size() / 3u;
        if (boneCount == 0 || mesh.skinWeights.size() != vertexCount * 8u) {
            std::fprintf(stderr,
                         "[FBXParser] mesh '%s' has invalid skin payload "
                         "(vertices=%zu floats=%zu bones=%zu)\n",
                         mesh.name.c_str(), vertexCount,
                         mesh.skinWeights.size(), boneCount);
            return false;
        }
        for (std::size_t v = 0; v < vertexCount; ++v) {
            float sum = 0.0f;
            for (std::size_t slot = 0; slot < 4; ++slot) {
                const float indexValue = mesh.skinWeights[v * 8u + slot];
                const float weight = mesh.skinWeights[v * 8u + 4u + slot];
                const UInt32 index = static_cast<UInt32>(indexValue + 0.5f);
                if (weight > 0.0f
                    && (index >= boneCount
                        || std::abs(indexValue - static_cast<float>(index)) > 0.001f)) {
                    std::fprintf(stderr,
                                 "[FBXParser] mesh '%s' vertex %zu references "
                                 "invalid bone %.3f/%zu\n",
                                 mesh.name.c_str(), v, indexValue, boneCount);
                    return false;
                }
                sum += weight;
            }
            if (std::abs(sum - 1.0f) > 0.002f) {
                std::fprintf(stderr,
                             "[FBXParser] mesh '%s' vertex %zu weight sum %.6f\n",
                             mesh.name.c_str(), v, sum);
                return false;
            }
        }
    }
    return true;
}

void FBXParser::_collectSkeletonBones(const aiNode* node, int parentIndex,
                                      const std::unordered_set<std::string>& boneNodeNames,
                                      SkeletonData& skeleton,
                                      const ayt::math::Float4x4& fromParentBone) {
    if (!node) return;

    std::string nodeName = node->mName.C_Str();
    const ayt::math::Float4x4 local =
        fromParentBone * toAyMatrix(node->mTransformation);

    // 检查是否是骨骼节点
    bool isBone = boneNodeNames.find(nodeName) != boneNodeNames.end();

    int thisIndex = -1;
    if (isBone) {
        BoneData bone;
        bone.name = nodeName;
        bone.parentIndex = parentIndex;

        // The offset is defined by the skin cluster, not by inverse(local).
        // Using inverse(node local) only worked accidentally for one-bone
        // hierarchies and broke bind pose as soon as parents were present.
        const auto offset = _boneOffsets.find(nodeName);
        bone.inverseBindMatrix = offset != _boneOffsets.end()
            ? offset->second : local.inverse();

        // Fold non-bone ancestors between this bone and its nearest runtime
        // bone parent into one local transform. This preserves FBX armature/
        // pivot nodes without adding palette slots that meshes never index.
        if (!local.decompose(bone.localPosition,
                             bone.localRotation,
                             bone.localScale)) {
            bone.localPosition = ayt::math::FVector3(0, 0, 0);
            bone.localRotation = ayt::math::FQuaternion::identity();
            bone.localScale = ayt::math::FVector3(1, 1, 1);
        }

        const auto mapped = _boneNameToIndex.find(nodeName);
        if (mapped != _boneNameToIndex.end()
            && mapped->second < skeleton.bones.size()) {
            thisIndex = static_cast<int>(mapped->second);
            skeleton.bones[mapped->second] = std::move(bone);
        }
    }

    // 递归处理子节点
    const ayt::math::Float4x4 childAccumulator = isBone
        ? ayt::math::Float4x4::identity() : local;
    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        _collectSkeletonBones(node->mChildren[i],
                              isBone ? thisIndex : parentIndex,
                              boneNodeNames, skeleton, childAccumulator);
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
