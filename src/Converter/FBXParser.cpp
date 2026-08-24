#include "AYResource/Converter/FBXParser.h"
#include "AYResource/MaterialTextureContract.h"
#include "AYResource/VirtualAssetPath.h"
#include "AYResource/assetsDefs/IMesh.h"
#include <AYMath/MathUtils.h>
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
#include <cmath>
#include <iterator>

namespace ayt::resource
{

namespace {

float normalizeSourceUvV(const SourceCoordinatePolicy& policy, float v)
{
    return policy.uvOrigin == ImportUvOrigin::BottomLeft ? 1.0f - v : v;
}

float normalizeSourceTangentHandedness(const SourceCoordinatePolicy& policy,
                                       float handedness)
{
    // Mirroring one UV axis reverses the bitangent direction.  Preserve the
    // tangent-space basis by flipping its serialized handedness exactly once.
    return policy.uvOrigin == ImportUvOrigin::BottomLeft
        ? -handedness : handedness;
}

} // namespace

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
    const bool manualCoordinates =
        _sourceCoordinates.mode == SourceCoordinateMode::Manual;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION,
                             manualCoordinates);
    importer.SetPropertyBool(AI_CONFIG_FBX_CONVERT_TO_M,
                             _sourceCoordinates.metersPerUnit <= 0.0f);

    unsigned int flags = aiProcess_Triangulate
                       | aiProcess_JoinIdenticalVertices
                       | aiProcess_LimitBoneWeights;
    // Auto mode retains Assimp's normalized left-handed output. Manual mode
    // loads the declared source basis unchanged; _applySourceCoordinatePolicy
    // then performs one explicit, auditable conversion across every asset
    // payload (not just mesh vertices).
    if (!manualCoordinates) {
        flags |= aiProcess_MakeLeftHanded | aiProcess_FlipWindingOrder;
    }
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

    std::fprintf(stderr,
                 "[FBXParser] UV contract source=%s target=top-left flipV=%d\n",
                 _sourceCoordinates.uvOrigin == ImportUvOrigin::BottomLeft
                     ? "bottom-left" : "top-left",
                 _sourceCoordinates.uvOrigin == ImportUvOrigin::BottomLeft
                     ? 1 : 0);

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

    if (manualCoordinates && !_applySourceCoordinatePolicy()) {
        return false;
    }

    return !_result->meshes.empty();
}

std::unique_ptr<IntermediateAsset> FBXParser::getResult() {
    return std::move(_result);
}

namespace {

ayt::math::FVector3 axisVector(ImportAxis axis)
{
    switch (axis) {
    case ImportAxis::PositiveX: return { 1.0f, 0.0f, 0.0f};
    case ImportAxis::NegativeX: return {-1.0f, 0.0f, 0.0f};
    case ImportAxis::PositiveY: return { 0.0f, 1.0f, 0.0f};
    case ImportAxis::NegativeY: return { 0.0f,-1.0f, 0.0f};
    case ImportAxis::PositiveZ: return { 0.0f, 0.0f, 1.0f};
    case ImportAxis::NegativeZ: return { 0.0f, 0.0f,-1.0f};
    }
    return {0.0f, 0.0f, 0.0f};
}

float basisDeterminant(const ayt::math::FVector3& right,
                       const ayt::math::FVector3& up,
                       const ayt::math::FVector3& forward)
{
    return right.dot(up.cross(forward));
}

ayt::math::Float4x4 basisMatrix(const ayt::math::FVector3& right,
                                const ayt::math::FVector3& up,
                                const ayt::math::FVector3& forward,
                                float scale)
{
    return ayt::math::Float4x4(
        scale * right.x,   scale * right.y,   scale * right.z,   0.0f,
        scale * up.x,      scale * up.y,      scale * up.z,      0.0f,
        scale * forward.x, scale * forward.y, scale * forward.z, 0.0f,
        0.0f,              0.0f,              0.0f,              1.0f);
}

ayt::math::FVector3 absoluteBasisTransform(
    const ayt::math::FVector3& right,
    const ayt::math::FVector3& up,
    const ayt::math::FVector3& forward,
    const ayt::math::FVector3& value)
{
    const ayt::math::FVector3 ar(std::abs(right.x), std::abs(right.y), std::abs(right.z));
    const ayt::math::FVector3 au(std::abs(up.x), std::abs(up.y), std::abs(up.z));
    const ayt::math::FVector3 af(std::abs(forward.x), std::abs(forward.y), std::abs(forward.z));
    return {ar.dot(value), au.dot(value), af.dot(value)};
}

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

bool FBXParser::_applySourceCoordinatePolicy()
{
    if (!_result) return false;
    if (!std::isfinite(_sourceCoordinates.metersPerUnit)
        || _sourceCoordinates.metersPerUnit < 0.0f) {
        std::fprintf(stderr, "[FBXParser] invalid metersPerUnit %.9g\n",
                     _sourceCoordinates.metersPerUnit);
        return false;
    }

    const ayt::math::FVector3 up = axisVector(_sourceCoordinates.up);
    const ayt::math::FVector3 forward = axisVector(_sourceCoordinates.forward);
    if (std::abs(up.dot(forward)) > 0.5f) {
        std::fprintf(stderr,
                     "[FBXParser] manual Up and Forward axes must be orthogonal\n");
        return false;
    }

    ayt::math::FVector3 right = up.cross(forward);
    if (_sourceCoordinates.handedness == ImportHandedness::Right) {
        right = -right;
    }
    const float determinant = basisDeterminant(right, up, forward);
    const float unitScale = _sourceCoordinates.metersPerUnit > 0.0f
        ? _sourceCoordinates.metersPerUnit : 1.0f;
    const ayt::math::Float4x4 basis = basisMatrix(right, up, forward, 1.0f);
    const ayt::math::Float4x4 sourceToEngine =
        basisMatrix(right, up, forward, unitScale);
    const ayt::math::Float4x4 engineToSource = sourceToEngine.inverse();

    for (MeshData& mesh : _result->meshes) {
        float minBounds[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float maxBounds[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
            const ayt::math::FVector3 p = sourceToEngine.transformDirection(
                {mesh.positions[i], mesh.positions[i + 1], mesh.positions[i + 2]});
            mesh.positions[i] = p.x; mesh.positions[i + 1] = p.y; mesh.positions[i + 2] = p.z;
            minBounds[0] = std::min(minBounds[0], p.x);
            minBounds[1] = std::min(minBounds[1], p.y);
            minBounds[2] = std::min(minBounds[2], p.z);
            maxBounds[0] = std::max(maxBounds[0], p.x);
            maxBounds[1] = std::max(maxBounds[1], p.y);
            maxBounds[2] = std::max(maxBounds[2], p.z);
        }
        for (std::size_t i = 0; i + 2 < mesh.normals.size(); i += 3) {
            const ayt::math::FVector3 n = basis.transformDirection(
                {mesh.normals[i], mesh.normals[i + 1], mesh.normals[i + 2]}).normalize();
            mesh.normals[i] = n.x; mesh.normals[i + 1] = n.y; mesh.normals[i + 2] = n.z;
        }
        for (std::size_t i = 0; i + 3 < mesh.tangents.size(); i += 4) {
            const ayt::math::FVector3 t = basis.transformDirection(
                {mesh.tangents[i], mesh.tangents[i + 1], mesh.tangents[i + 2]}).normalize();
            mesh.tangents[i] = t.x; mesh.tangents[i + 1] = t.y; mesh.tangents[i + 2] = t.z;
            if (determinant < 0.0f) mesh.tangents[i + 3] = -mesh.tangents[i + 3];
        }
        for (MorphTargetData& target : mesh.morphTargets) {
            for (MorphVertexDelta& delta : target.deltas) {
                const ayt::math::FVector3 p = sourceToEngine.transformDirection(
                    {delta.positionDelta[0], delta.positionDelta[1], delta.positionDelta[2]});
                delta.positionDelta[0] = p.x;
                delta.positionDelta[1] = p.y;
                delta.positionDelta[2] = p.z;

                const ayt::math::FVector3 n = basis.transformDirection(
                    {delta.normalDelta[0], delta.normalDelta[1], delta.normalDelta[2]});
                delta.normalDelta[0] = n.x;
                delta.normalDelta[1] = n.y;
                delta.normalDelta[2] = n.z;

                const ayt::math::FVector3 t = basis.transformDirection(
                    {delta.tangentDelta[0], delta.tangentDelta[1], delta.tangentDelta[2]});
                delta.tangentDelta[0] = t.x;
                delta.tangentDelta[1] = t.y;
                delta.tangentDelta[2] = t.z;
                if (determinant < 0.0f) {
                    delta.tangentDelta[3] = -delta.tangentDelta[3];
                }
            }
        }
        if (!mesh.positions.empty()) {
            for (int c = 0; c < 3; ++c) {
                mesh.boundsMin[c] = minBounds[c];
                mesh.boundsMax[c] = maxBounds[c];
            }
        }
        if (determinant < 0.0f) {
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
            }
        }
    }

    for (SkeletonData& skeleton : _result->skeletons) {
        for (BoneData& bone : skeleton.bones) {
            bone.inverseBindMatrix = sourceToEngine * bone.inverseBindMatrix * engineToSource;
            const ayt::math::Float4x4 local = ayt::math::Float4x4::fromTRS(
                bone.localPosition, bone.localRotation, bone.localScale);
            const ayt::math::Float4x4 converted =
                sourceToEngine * local * engineToSource;
            if (!converted.decompose(bone.localPosition,
                                     bone.localRotation,
                                     bone.localScale)) {
                std::fprintf(stderr,
                             "[FBXParser] coordinate conversion produced singular bone '%s'\n",
                             bone.name.c_str());
                return false;
            }
        }
    }

    const ayt::math::Float4x4 inverseBasis = basis.inverse();
    for (AnimationData& animation : _result->animations) {
        for (KeyframeTrack& track : animation.tracks) {
            if (track.property == "position") {
                for (std::size_t i = 0; i + 2 < track.values.size(); i += 3) {
                    const ayt::math::FVector3 v = sourceToEngine.transformDirection(
                        {track.values[i], track.values[i + 1], track.values[i + 2]});
                    track.values[i] = v.x; track.values[i + 1] = v.y; track.values[i + 2] = v.z;
                }
            } else if (track.property == "scale") {
                for (std::size_t i = 0; i + 2 < track.values.size(); i += 3) {
                    const ayt::math::FVector3 v = absoluteBasisTransform(
                        right, up, forward,
                        {track.values[i], track.values[i + 1], track.values[i + 2]});
                    track.values[i] = v.x; track.values[i + 1] = v.y; track.values[i + 2] = v.z;
                }
            } else if (track.property == "rotation") {
                for (std::size_t i = 0; i + 3 < track.values.size(); i += 4) {
                    const ayt::math::FQuaternion q(track.values[i], track.values[i + 1],
                                                   track.values[i + 2], track.values[i + 3]);
                    const ayt::math::Float4x4 converted =
                        basis * q.toMatrix() * inverseBasis;
                    const ayt::math::FQuaternion out =
                        ayt::math::FQuaternion_cast(converted).normalize();
                    track.values[i] = out.x; track.values[i + 1] = out.y;
                    track.values[i + 2] = out.z; track.values[i + 3] = out.w;
                }
            }
        }
    }

    std::fprintf(stderr,
                 "[FBXParser] explicit source coordinates applied tag='%s' det=%.0f unit=%.9g\n",
                 sourceCoordinatePolicyCacheTag(_sourceCoordinates).c_str(),
                 determinant, unitScale);
    return true;
}

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
    const SourceCoordinatePolicy& sourceCoordinates,
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
                mesh.uvs[(vertexOffset + v) * 2 + 1] = normalizeSourceUvV(
                    sourceCoordinates, m->mTextureCoords[0][v].y);
            }
            break;
        case static_cast<UInt8>(MeshAttribute::Tangent):
            for (UInt32 v = 0; v < vertexCount; v++) {
                mesh.tangents[(vertexOffset + v) * 4 + 0] = m->mTangents[v].x;
                mesh.tangents[(vertexOffset + v) * 4 + 1] = m->mTangents[v].y;
                mesh.tangents[(vertexOffset + v) * 4 + 2] = m->mTangents[v].z;
                const float handedness =
                    (m->mNormals[v] ^ m->mTangents[v]) * m->mBitangents[v]
                        > 0 ? 1.0f : -1.0f;
                mesh.tangents[(vertexOffset + v) * 4 + 3] =
                    normalizeSourceTangentHandedness(sourceCoordinates,
                                                     handedness);
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
                mesh.uvs[(vertexOffset + v) * 2 + 1] = normalizeSourceUvV(
                    _sourceCoordinates, m->mTextureCoords[0][v].y);
            }
        }

        // 复制切线
        if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
            for (unsigned int v = 0; v < m->mNumVertices; v++) {
                mesh.tangents[(vertexOffset + v) * 4 + 0] = m->mTangents[v].x;
                mesh.tangents[(vertexOffset + v) * 4 + 1] = m->mTangents[v].y;
                mesh.tangents[(vertexOffset + v) * 4 + 2] = m->mTangents[v].z;
                const float handedness =
                    (m->mNormals[v] ^ m->mTangents[v]) * m->mBitangents[v]
                        > 0 ? 1.0f : -1.0f;
                mesh.tangents[(vertexOffset + v) * 4 + 3] =
                    normalizeSourceTangentHandedness(_sourceCoordinates,
                                                     handedness);
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
        submesh.sourceMaterialIndex = static_cast<UInt32>(m->mMaterialIndex);
        mesh.submeshes.push_back(submesh);

        // Material slot — must match MaterialConverter / FBXConverter contract.
        const std::string base = _assetBaseName.empty() ? "asset" : _assetBaseName;
        mesh.materialSlots.push_back(
            makeMaterialVirtualPath(base, static_cast<std::size_t>(m->mMaterialIndex)));

        _parseMorphTargets(m, vertexOffset, mesh);

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
                    mesh.uvs[(vertexOffset + v) * 2 + 1] = normalizeSourceUvV(
                        _sourceCoordinates, m->mTextureCoords[0][v].y);
                }
            }

            // 复制切线
            if (mesh.attributeMask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
                for (unsigned int v = 0; v < m->mNumVertices; v++) {
                    mesh.tangents[(vertexOffset + v) * 4 + 0] = m->mTangents[v].x;
                    mesh.tangents[(vertexOffset + v) * 4 + 1] = m->mTangents[v].y;
                    mesh.tangents[(vertexOffset + v) * 4 + 2] = m->mTangents[v].z;
                    const float handedness =
                        (m->mNormals[v] ^ m->mTangents[v]) * m->mBitangents[v]
                            > 0 ? 1.0f : -1.0f;
                    mesh.tangents[(vertexOffset + v) * 4 + 3] =
                        normalizeSourceTangentHandedness(_sourceCoordinates,
                                                         handedness);
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
            submesh.sourceMaterialIndex = static_cast<UInt32>(m->mMaterialIndex);
            mesh.submeshes.push_back(submesh);

            // Material slot — must match MaterialConverter / FBXConverter contract.
            const std::string base = _assetBaseName.empty() ? "asset" : _assetBaseName;
            mesh.materialSlots.push_back(
                makeMaterialVirtualPath(base, static_cast<std::size_t>(m->mMaterialIndex)));

            _parseMorphTargets(m, vertexOffset, mesh);

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
        // Prefer explicit PBR semantics when both legacy and PBR aliases
        // exist. The first successful source owns the public material slot.
        {aiTextureType_BASE_COLOR, "baseColorTexture"},
        {aiTextureType_DIFFUSE, "baseColorTexture"},
        {aiTextureType_NORMAL_CAMERA, "normalTexture"},
        {aiTextureType_NORMALS, "normalTexture"},
        {aiTextureType_SPECULAR, "specularTexture"},
        {aiTextureType_EMISSION_COLOR, "emissiveTexture"},
        {aiTextureType_EMISSIVE, "emissiveTexture"},
        {aiTextureType_HEIGHT, "heightTexture"},
        {aiTextureType_OPACITY, "opacityTexture"},
        {aiTextureType_METALNESS, "metallicTexture"},
        {aiTextureType_DIFFUSE_ROUGHNESS, "roughnessTexture"},
        {aiTextureType_AMBIENT_OCCLUSION, "aoTexture"},
        {aiTextureType_REFLECTION, "reflectionTexture"},
        {aiTextureType_UNKNOWN, "unknownTexture"},
    };

    for (const auto& [texType, paramName] : textureTypes) {
        const bool slotAlreadyAssigned = std::any_of(
            material.parameters.begin(), material.parameters.end(),
            [paramName](const Param& param) {
                return param.type == MaterialParamType::Texture2D
                    && param.name == paramName;
            });
        if (slotAlreadyAssigned) {
            continue;
        }
        // 检查是否有该类型的纹理
        aiTextureType mappedType = texType;
        unsigned int texCount = mat->GetTextureCount(mappedType);
        if (texCount == 0) continue;

        // 获取第一个纹理路径
        aiString texPath;
        if (mat->GetTexture(mappedType, 0, &texPath) == AI_SUCCESS) {
            std::string path(texPath.C_Str());
            if (path.empty()) continue;

            // Blender/FBX commonly publishes the same RGBA image as both
            // Diffuse and TransparencyFactor.  Keep one base-color binding
            // and consume its alpha exactly once.  This check must use the
            // original Assimp path; the generated _d/_o virtual paths are
            // intentionally different and cannot prove source identity.
            if (std::string_view(paramName) == "opacityTexture") {
                const bool aliasesBaseColor = std::any_of(
                    material.textureSources.begin(), material.textureSources.end(),
                    [&path](const MaterialData::TextureSource& source) {
                        return source.parameterName == "baseColorTexture"
                            && sameMaterialTextureSource(source.sourcePath, path);
                    });
                if (aliasesBaseColor) {
                    continue;
                }
            }

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

            const MaterialTextureContract contract =
                materialTextureContract(paramName);

            Param param;
            param.name = paramName;
            param.type = MaterialParamType::Texture2D;
            param.texturePath = makeTextureVirtualPath(textureName, contract.usageSuffix,
                                                       texExt.c_str());
            material.parameters.push_back(param);

            material.texturePaths.push_back(path);
            MaterialData::TextureSource source;
            source.parameterName = paramName;
            source.sourcePath = path;
            source.virtualPath = param.texturePath;
            source.usageSuffix = contract.usageSuffix;
            source.colorSpace = contract.colorSpace;
            source.normalY = contract.normalY;
            material.textureSources.push_back(std::move(source));
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

    // Roughness: prefer an authored PBR value. Legacy FBX/Phong exports
    // usually carry only shininess, which is converted to perceptual
    // roughness instead of being silently replaced with 0.5.
    float roughness = 0.5f;
    float legacyShininess = 0.0f;
    const bool hasLegacyShininess =
        mat->Get(AI_MATKEY_SHININESS, legacyShininess) == AI_SUCCESS;
    aiString roughnessStr;
    const bool hasPbrRoughness =
        mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS;
    bool parsedStringRoughness = false;
    if (!hasPbrRoughness &&
        (mat->Get("$mat.pbrRoughnessFactor", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("roughness", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("Roughness", 0, 0, roughnessStr) == AI_SUCCESS ||
        mat->Get("$mat.glmModelPBR.roughness", 0, 0, roughnessStr) == AI_SUCCESS)) {
        try {
            const float parsed = std::stof(roughnessStr.C_Str());
            if (parsed >= 0.0f && parsed <= 1.0f) {
                roughness = parsed;
                parsedStringRoughness = true;
            }
        } catch (...) {}
    }
    if (!hasPbrRoughness && !parsedStringRoughness && hasLegacyShininess) {
        roughness = std::sqrt(2.0f / (std::max(0.0f, legacyShininess) + 2.0f));
    }
    roughness = std::clamp(roughness, 0.045f, 1.0f);
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

    // FBX does not reliably identify either convention. Make the importer
    // default explicit in .aymat so a project/import preset can override it.
    Param normalYParam;
    normalYParam.name = "normalYSign";
    normalYParam.type = MaterialParamType::Float;
    normalYParam.floatValue = 1.0f;
    material.parameters.push_back(normalYParam);
    Param premultipliedParam;
    premultipliedParam.name = "premultipliedAlpha";
    premultipliedParam.type = MaterialParamType::Float;
    premultipliedParam.floatValue = 0.0f;
    material.parameters.push_back(premultipliedParam);

    // 提取纹理路径
    _extractMaterialTextures(mat, material);

    // Alpha-channel selection is a serialized material property, not a
    // shader convention.  No dedicated texture means BaseColor.a owns alpha;
    // a distinct opacity image defaults to the conventional grayscale red
    // channel.  Import policy may remove a spurious opacity slot later and
    // will update this property in lockstep.
    const bool hasDedicatedOpacity = std::any_of(
        material.parameters.begin(), material.parameters.end(),
        [](const Param& param) {
            return param.type == MaterialParamType::Texture2D
                && param.name == "opacityTexture";
        });
    Param opacitySourceParam;
    opacitySourceParam.name = "opacitySource";
    opacitySourceParam.type = MaterialParamType::Float;
    opacitySourceParam.floatValue = materialOpacitySourceValue(
        hasDedicatedOpacity ? MaterialOpacitySource::TextureRed
                            : MaterialOpacitySource::BaseColorAlpha);
    material.parameters.push_back(opacitySourceParam);

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
    if (hasLegacyShininess) {
        Param param;
        param.name = "shininess";
        param.type = MaterialParamType::Float;
        param.floatValue = legacyShininess;
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

void FBXParser::_parseMorphTargets(const aiMesh* sourceMesh,
                                  UInt32 vertexOffset,
                                  MeshData& mesh) {
    if (!sourceMesh || sourceMesh->mNumAnimMeshes == 0) {
        return;
    }

    const bool hasNormals = sourceMesh->HasNormals() && sourceMesh->mNormals != nullptr;
    const bool hasTangents = sourceMesh->HasTangentsAndBitangents()
                            && sourceMesh->mTangents != nullptr
                            && sourceMesh->mBitangents != nullptr;

    for (unsigned int mi = 0; mi < sourceMesh->mNumAnimMeshes; ++mi) {
        const aiAnimMesh* animMesh = sourceMesh->mAnimMeshes[mi];
        if (!animMesh) {
            continue;
        }

        if (animMesh->mNumVertices != sourceMesh->mNumVertices) {
            // Malformed animation mesh (different topology): skip this target.
            continue;
        }

        std::string targetName = animMesh->mName.C_Str();
        if (targetName.empty()) {
            targetName = "morph_" + std::to_string(mi);
        }

        auto existing = std::find_if(mesh.morphTargets.begin(), mesh.morphTargets.end(),
            [&](const MorphTargetData& candidate) { return candidate.name == targetName; });
        const bool createdTarget = existing == mesh.morphTargets.end();
        if (existing == mesh.morphTargets.end()) {
            MorphTargetData created;
            created.name = targetName;
            // aiAnimMesh::mWeight is not a portable bind/default weight for
            // FBX. In particular, Assimp maps FBX BlendShapeChannel
            // FullWeights (normally 100%) to 1.0, so using it here would
            // activate every shape key at load time. The neutral FBX mesh is
            // the base mesh; animation channels or an explicit runtime API
            // opt individual targets in from zero.
            created.defaultWeight = 0.0f;
            mesh.morphTargets.push_back(std::move(created));
            existing = std::prev(mesh.morphTargets.end());
        }
        MorphTargetData& target = *existing;
        const std::size_t deltaCountBefore = target.deltas.size();

        target.deltas.reserve(target.deltas.size() + sourceMesh->mNumVertices);
        for (UInt32 v = 0; v < sourceMesh->mNumVertices; ++v) {
            const auto basePos = sourceMesh->mVertices[v];
            const auto animPos = animMesh->mVertices != nullptr
                ? animMesh->mVertices[v]
                : basePos;
            const float px = animPos.x - basePos.x;
            const float py = animPos.y - basePos.y;
            const float pz = animPos.z - basePos.z;

            float nx = 0.0f, ny = 0.0f, nz = 0.0f;
            bool hasNormalDelta = false;
            if (hasNormals && animMesh->mNormals) {
                const auto baseNormal = sourceMesh->mNormals[v];
                const auto animNormal = animMesh->mNormals[v];
                nx = animNormal.x - baseNormal.x;
                ny = animNormal.y - baseNormal.y;
                nz = animNormal.z - baseNormal.z;
                hasNormalDelta = (std::abs(nx) > 0.0001f)
                                 || (std::abs(ny) > 0.0001f)
                                 || (std::abs(nz) > 0.0001f);
            }

            float tx = 0.0f, ty = 0.0f, tz = 0.0f, tw = 0.0f;
            bool hasTangentDelta = false;
            if (hasTangents && animMesh->mTangents) {
                const auto baseTangent = sourceMesh->mTangents[v];
                const auto animTangent = animMesh->mTangents[v];
                tx = animTangent.x - baseTangent.x;
                ty = animTangent.y - baseTangent.y;
                tz = animTangent.z - baseTangent.z;
                hasTangentDelta = (std::abs(tx) > 0.0001f)
                                  || (std::abs(ty) > 0.0001f)
                                  || (std::abs(tz) > 0.0001f);

                if (sourceMesh->mBitangents != nullptr && animMesh->mBitangents != nullptr) {
                    const float baseHandedness =
                        (sourceMesh->mNormals[v].x * (baseTangent.y * sourceMesh->mBitangents[v].z
                                                      - baseTangent.z * sourceMesh->mBitangents[v].y)
                         + sourceMesh->mNormals[v].y * (baseTangent.z * sourceMesh->mBitangents[v].x
                                                       - baseTangent.x * sourceMesh->mBitangents[v].z)
                         + sourceMesh->mNormals[v].z * (baseTangent.x * sourceMesh->mBitangents[v].y
                                                       - baseTangent.y * sourceMesh->mBitangents[v].x)) > 0.0f
                        ? 1.0f
                        : -1.0f;
                    const float animHandedness =
                        (animMesh->mNormals != nullptr ? animMesh->mNormals[v].x : sourceMesh->mNormals[v].x)
                        * (animTangent.y * animMesh->mBitangents[v].z - animTangent.z * animMesh->mBitangents[v].y)
                        + (animMesh->mNormals != nullptr ? animMesh->mNormals[v].y : sourceMesh->mNormals[v].y)
                        * (animTangent.z * animMesh->mBitangents[v].x - animTangent.x * animMesh->mBitangents[v].z)
                        + (animMesh->mNormals != nullptr ? animMesh->mNormals[v].z : sourceMesh->mNormals[v].z)
                        * (animTangent.x * animMesh->mBitangents[v].y - animTangent.y * animMesh->mBitangents[v].x) > 0.0f
                        ? 1.0f
                        : -1.0f;
                    tw = animHandedness - baseHandedness;
                    hasTangentDelta = hasTangentDelta || (std::abs(tw) > 0.0001f);
                }
            }

            const bool hasDelta = (std::abs(px) > 0.0001f)
                                || (std::abs(py) > 0.0001f)
                                || (std::abs(pz) > 0.0001f)
                                || hasNormalDelta
                                || hasTangentDelta;
            if (!hasDelta) {
                continue;
            }

            MorphVertexDelta delta;
            delta.vertexIndex = vertexOffset + v;
            delta.positionDelta[0] = px;
            delta.positionDelta[1] = py;
            delta.positionDelta[2] = pz;
            if (hasNormalDelta) {
                delta.normalDelta[0] = nx;
                delta.normalDelta[1] = ny;
                delta.normalDelta[2] = nz;
            }
            if (hasTangentDelta) {
                delta.tangentDelta[0] = tx;
                delta.tangentDelta[1] = ty;
                delta.tangentDelta[2] = tz;
                delta.tangentDelta[3] = tw;
            }

            target.deltas.push_back(std::move(delta));
        }

        if (createdTarget && target.deltas.size() == deltaCountBefore) {
            mesh.morphTargets.erase(existing);
        }
    }
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

        // Shape-key animation is independent from skeletal TRS animation.
        // Preserve it as ordinary Float tracks so AnimationPlayer's existing
        // orphan-float sink can route weights to a mesh runtime. The property
        // carries the stable target name; targetNode identifies the Assimp
        // morph channel / source mesh.
        for (unsigned int ci = 0; ci < anim->mNumMorphMeshChannels; ++ci) {
            const aiMeshMorphAnim* channel = anim->mMorphMeshChannels[ci];
            if (channel == nullptr || channel->mKeys == nullptr || channel->mNumKeys == 0) {
                continue;
            }

            const std::string channelName = channel->mName.C_Str();
            std::unordered_set<unsigned int> targetIndices;
            for (unsigned int keyIndex = 0; keyIndex < channel->mNumKeys; ++keyIndex) {
                const aiMeshMorphKey& key = channel->mKeys[keyIndex];
                if (key.mValues == nullptr || key.mWeights == nullptr) {
                    continue;
                }
                for (unsigned int valueIndex = 0;
                     valueIndex < key.mNumValuesAndWeights;
                     ++valueIndex) {
                    targetIndices.insert(key.mValues[valueIndex]);
                }
            }

            auto targetNameForIndex = [&](unsigned int targetIndex) {
                std::string fallback = "morph_" + std::to_string(targetIndex);
                std::string uniqueCandidate;
                bool candidateAmbiguous = false;
                for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
                    const aiMesh* sourceMesh = scene->mMeshes[meshIndex];
                    if (sourceMesh == nullptr || targetIndex >= sourceMesh->mNumAnimMeshes
                        || sourceMesh->mAnimMeshes[targetIndex] == nullptr) {
                        continue;
                    }
                    std::string candidate = sourceMesh->mAnimMeshes[targetIndex]->mName.C_Str();
                    if (candidate.empty()) {
                        candidate = fallback;
                    }
                    if (channelName == sourceMesh->mName.C_Str()) {
                        return candidate;
                    }
                    if (uniqueCandidate.empty()) {
                        uniqueCandidate = candidate;
                    } else if (uniqueCandidate != candidate) {
                        candidateAmbiguous = true;
                    }
                }
                return !uniqueCandidate.empty() && !candidateAmbiguous
                    ? uniqueCandidate : fallback;
            };

            std::vector<unsigned int> orderedTargetIndices(targetIndices.begin(),
                                                           targetIndices.end());
            std::sort(orderedTargetIndices.begin(), orderedTargetIndices.end());
            for (const unsigned int targetIndex : orderedTargetIndices) {
                KeyframeTrack track;
                track.targetNode = channelName;
                track.property = "morph:" + targetNameForIndex(targetIndex);
                track.valueType = AnimTrackType::Float;
                track.times.reserve(channel->mNumKeys);
                track.values.reserve(channel->mNumKeys);

                for (unsigned int keyIndex = 0; keyIndex < channel->mNumKeys; ++keyIndex) {
                    const aiMeshMorphKey& key = channel->mKeys[keyIndex];
                    Float32 weight = 0.0f;
                    if (key.mValues != nullptr && key.mWeights != nullptr) {
                        for (unsigned int valueIndex = 0;
                             valueIndex < key.mNumValuesAndWeights;
                             ++valueIndex) {
                            if (key.mValues[valueIndex] == targetIndex) {
                                weight = static_cast<Float32>(key.mWeights[valueIndex]);
                                break;
                            }
                        }
                    }
                    track.times.push_back(static_cast<Float32>(key.mTime / ticks));
                    track.values.push_back(weight);
                }

                data.tracks.push_back(std::move(track));
            }
        }

        // 空 take (零轨道) 不加入 — 让 AnimationConverter 走"无 anim"分支
        if (!data.tracks.empty()) {
            _result->animations.push_back(std::move(data));
        }
    }
}

} // namespace ayt::resource
