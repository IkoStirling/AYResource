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
#include <string_view>
#include <iomanip>
#include <limits>
#include <sstream>
#include <cctype>

namespace ayt::resource
{

namespace {

bool isFbxPath(std::string_view path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return false;
    std::string extension(path.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".fbx";
}

bool nodeReferencesMeshKind(const aiNode* node, const aiScene* scene,
                            bool requireSkinning)
{
    if (!node || !scene) return false;
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        const unsigned meshIndex = node->mMeshes[i];
        if (meshIndex >= scene->mNumMeshes || !scene->mMeshes[meshIndex]) {
            continue;
        }
        if (!requireSkinning || scene->mMeshes[meshIndex]->HasBones()) {
            return true;
        }
    }
    return false;
}

bool findReferenceMeshTransform(const aiNode* node, const aiScene* scene,
                                const aiMatrix4x4& parentWorld,
                                bool requireSkinning, aiMatrix4x4& out)
{
    if (!node) return false;
    const aiMatrix4x4 world = parentWorld * node->mTransformation;
    if (nodeReferencesMeshKind(node, scene, requireSkinning)) {
        out = world;
        return true;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        if (findReferenceMeshTransform(node->mChildren[i], scene, world,
                                       requireSkinning, out)) {
            return true;
        }
    }
    return false;
}

const aiNode* findNodeByName(const aiNode* node, std::string_view name)
{
    if (!node) return nullptr;
    if (node->mName.C_Str() == name) return node;
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        if (const aiNode* found = findNodeByName(node->mChildren[i], name)) {
            return found;
        }
    }
    return nullptr;
}

aiMatrix4x4 nodeWorldTransform(const aiNode* node)
{
    aiMatrix4x4 world;
    if (!node) return world;
    std::vector<const aiNode*> path;
    for (const aiNode* current = node; current; current = current->mParent) {
        path.push_back(current);
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        world *= (*it)->mTransformation;
    }
    return world;
}

bool findAutoReferenceTransform(const aiScene* scene, aiMatrix4x4& out)
{
    if (!scene || !scene->mRootNode) return false;
    const aiMatrix4x4 identity;
    if (findReferenceMeshTransform(scene->mRootNode, scene, identity,
                                   true, out)
        || findReferenceMeshTransform(scene->mRootNode, scene, identity,
                                      false, out)) {
        return true;
    }

    // Animation-only FBX files need the same wrapper resolution even when the
    // exporter omits render geometry.  Use the top-level owner of the first
    // animated channel rather than a bone-local transform.
    for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* animation = scene->mAnimations[ai];
        if (!animation) continue;
        for (unsigned ci = 0; ci < animation->mNumChannels; ++ci) {
            const aiNodeAnim* channel = animation->mChannels[ci];
            if (!channel) continue;
            const aiNode* node = findNodeByName(
                scene->mRootNode, channel->mNodeName.C_Str());
            if (!node) continue;
            while (node->mParent && node->mParent != scene->mRootNode) {
                node = node->mParent;
            }
            out = nodeWorldTransform(node);
            return true;
        }
    }

    if (scene->mRootNode->mNumChildren > 0) {
        out = nodeWorldTransform(scene->mRootNode->mChildren[0]);
        return true;
    }
    out = scene->mRootNode->mTransformation;
    return true;
}

ayt::math::Float4x4 toAyMatrix(const aiMatrix4x4& source);

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

void auditImportedMeshWinding(const IntermediateAsset& asset)
{
    for (const MeshData& mesh : asset.meshes) {
        const MeshWindingAudit audit = auditCanonicalMeshWinding(
            mesh.positions, mesh.normals, mesh.indices);
        if (audit.invalidIndexTriangleCount == 0u
            && audit.mismatchedTriangleCount == 0u) {
            continue;
        }
        std::fprintf(stderr,
                     "[FBXParser] winding audit mesh='%s' triangles=%zu "
                     "compared=%zu mismatched=%zu degenerate=%zu invalid=%zu; "
                     "expected LH/CW-front canonical indices\n",
                     mesh.name.c_str(), audit.triangleCount,
                     audit.comparedTriangleCount,
                     audit.mismatchedTriangleCount,
                     audit.degenerateTriangleCount,
                     audit.invalidIndexTriangleCount);
    }
}

void setFloatParam(MaterialData& material, const char* name, float value)
{
    Param param;
    param.name = name;
    param.type = MaterialParamType::Float;
    param.floatValue = value;
    material.parameters.push_back(std::move(param));
}

void setFloat3Param(MaterialData& material, const char* name,
                    float x, float y, float z)
{
    Param param;
    param.name = name;
    param.type = MaterialParamType::Float3;
    param.float3Value[0] = x;
    param.float3Value[1] = y;
    param.float3Value[2] = z;
    material.parameters.push_back(std::move(param));
}

MaterialShadingModel translateShadingModel(aiShadingMode mode)
{
    switch (mode) {
    case aiShadingMode_Flat: return MaterialShadingModel::Flat;
    case aiShadingMode_Gouraud: return MaterialShadingModel::Gouraud;
    case aiShadingMode_Phong: return MaterialShadingModel::Phong;
    case aiShadingMode_Blinn: return MaterialShadingModel::Blinn;
    case aiShadingMode_Toon: return MaterialShadingModel::Toon;
    case aiShadingMode_OrenNayar: return MaterialShadingModel::OrenNayar;
    case aiShadingMode_Minnaert: return MaterialShadingModel::Minnaert;
    case aiShadingMode_CookTorrance: return MaterialShadingModel::CookTorrance;
    case aiShadingMode_NoShading: return MaterialShadingModel::Unlit;
    case aiShadingMode_Fresnel: return MaterialShadingModel::Fresnel;
    case aiShadingMode_PBR_BRDF: return MaterialShadingModel::Pbr;
    default: return MaterialShadingModel::Unknown;
    }
}

template<typename T>
bool getMaterialValue(const aiMaterial* material,
                      const char* key, unsigned int semantic,
                      unsigned int index, T& value)
{
    return material->Get(key, semantic, index, value) == AI_SUCCESS;
}

void appendOptionalFloat(const aiMaterial* source, MaterialData& material,
                         const char* parameterName, const char* key,
                         unsigned int semantic = 0, unsigned int index = 0)
{
    float value = 0.0f;
    if (getMaterialValue(source, key, semantic, index, value)) {
        setFloatParam(material, parameterName, value);
    }
}

void appendOptionalColor3(const aiMaterial* source, MaterialData& material,
                          const char* parameterName, const char* key,
                          unsigned int semantic = 0, unsigned int index = 0)
{
    aiColor3D value;
    if (getMaterialValue(source, key, semantic, index, value)) {
        setFloat3Param(material, parameterName, value.r, value.g, value.b);
    }
}

std::string sourcePropertyValue(const aiMaterial* material,
                                const aiMaterialProperty& property)
{
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    const auto appendArray = [&out](const auto* bytes, size_t byteCount,
                                    auto valueTag) {
        using Value = decltype(valueTag);
        const size_t count = byteCount / sizeof(Value);
        for (size_t i = 0; i < count; ++i) {
            Value value{};
            std::memcpy(&value, bytes + i * sizeof(Value), sizeof(Value));
            if (i != 0) out << ',';
            out << value;
        }
    };

    switch (property.mType) {
    case aiPTI_Float:
        appendArray(property.mData, property.mDataLength, float{});
        break;
    case aiPTI_Double:
        appendArray(property.mData, property.mDataLength, double{});
        break;
    case aiPTI_Integer:
        appendArray(property.mData, property.mDataLength, int{});
        break;
    case aiPTI_String: {
        aiString value;
        if (material->Get(property.mKey.C_Str(), property.mSemantic,
                          property.mIndex, value) == AI_SUCCESS) {
            out << value.C_Str();
        }
        break;
    }
    case aiPTI_Buffer:
    default:
        out << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < property.mDataLength; ++i) {
            out << std::setw(2)
                << static_cast<unsigned int>(
                       static_cast<unsigned char>(property.mData[i]));
        }
        break;
    }
    return out.str();
}

void captureSourceProperties(const aiMaterial* source, MaterialData& material)
{
    material.sourceAdapter = "assimp";
    material.sourceProperties.reserve(source->mNumProperties);
    for (unsigned int i = 0; i < source->mNumProperties; ++i) {
        const aiMaterialProperty* property = source->mProperties[i];
        if (!property) continue;
        MaterialData::SourceProperty captured;
        captured.key = property->mKey.C_Str();
        captured.semantic = property->mSemantic;
        captured.index = property->mIndex;
        switch (property->mType) {
        case aiPTI_Float:
            captured.type = MaterialSourcePropertyType::Float;
            break;
        case aiPTI_Double:
            captured.type = MaterialSourcePropertyType::Double;
            break;
        case aiPTI_String:
            captured.type = MaterialSourcePropertyType::String;
            break;
        case aiPTI_Integer:
            captured.type = MaterialSourcePropertyType::Integer;
            break;
        case aiPTI_Buffer:
        default:
            captured.type = MaterialSourcePropertyType::Buffer;
            break;
        }
        captured.value = sourcePropertyValue(source, *property);
        material.sourceProperties.push_back(std::move(captured));
    }
}

} // namespace

namespace detail {

namespace {

bool snapSignedAxis(const ayt::math::FVector3& value, ImportAxis& out)
{
    const float lengthSquared = value.dot(value);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12f) {
        return false;
    }
    const ayt::math::FVector3 normalized = value / std::sqrt(lengthSquared);
    const float components[3] = {normalized.x, normalized.y, normalized.z};
    unsigned major = 0;
    if (std::abs(components[1]) > std::abs(components[major])) major = 1;
    if (std::abs(components[2]) > std::abs(components[major])) major = 2;
    if (std::abs(components[major]) < 0.999f) return false;
    const bool positive = components[major] >= 0.0f;
    switch (major) {
    case 0: out = positive ? ImportAxis::PositiveX : ImportAxis::NegativeX; break;
    case 1: out = positive ? ImportAxis::PositiveY : ImportAxis::NegativeY; break;
    default: out = positive ? ImportAxis::PositiveZ : ImportAxis::NegativeZ; break;
    }
    return true;
}

ayt::math::FVector3 policyAxisVector(ImportAxis axis)
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

} // namespace

SourceCoordinatePolicy resolveFbxAutoCoordinatePolicy(
    const SourceCoordinatePolicy& requested,
    const ayt::math::Float4x4& referenceNodeTransform,
    bool* inferred)
{
    SourceCoordinatePolicy resolved = requested;
    resolved.mode = SourceCoordinateMode::Manual;
    resolved.up = ImportAxis::PositiveY;
    resolved.forward = ImportAxis::PositiveZ;
    resolved.handedness = ImportHandedness::Right;
    resolved.tag.clear();
    if (resolved.metersPerUnit == 0.0f) {
        // Assimp's FBX file-scale stage has already converted Auto imports to
        // metres.  Explicit user unit overrides remain authoritative.
        resolved.metersPerUnit = 1.0f;
    }

    ImportAxis rightAxis = ImportAxis::PositiveX;
    ImportAxis upAxis = ImportAxis::PositiveY;
    ImportAxis forwardAxis = ImportAxis::PositiveZ;
    const bool snapped =
        snapSignedAxis({referenceNodeTransform(0, 0),
                        referenceNodeTransform(0, 1),
                        referenceNodeTransform(0, 2)}, rightAxis)
        && snapSignedAxis({referenceNodeTransform(1, 0),
                           referenceNodeTransform(1, 1),
                           referenceNodeTransform(1, 2)}, upAxis)
        && snapSignedAxis({referenceNodeTransform(2, 0),
                           referenceNodeTransform(2, 1),
                           referenceNodeTransform(2, 2)}, forwardAxis);
    const ayt::math::FVector3 right = policyAxisVector(rightAxis);
    const ayt::math::FVector3 up = policyAxisVector(upAxis);
    const ayt::math::FVector3 forward = policyAxisVector(forwardAxis);
    const bool rightHandedBasis =
        right.dot(up.cross(forward)) > 0.999f;
    if (snapped && rightHandedBasis) {
        resolved.up = upAxis;
        resolved.forward = forwardAxis;
    }
    if (inferred) *inferred = snapped && rightHandedBasis;
    return resolved;
}

} // namespace detail

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
    // Assimp 6 applies FBX axis metadata to the scene root, but AY persists
    // mesh-local vertices and bind-space skeleton data without that node. Auto
    // therefore keeps Assimp right-handed until AY can bake the reference FBX
    // wrapper across mesh, skeleton, morph and animation payloads together.
    const bool manualCoordinates =
        _sourceCoordinates.mode == SourceCoordinateMode::Manual;
    const bool autoFbxCoordinates = !manualCoordinates && isFbxPath(_sourcePath);
    const bool explicitCoordinates = manualCoordinates || autoFbxCoordinates;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION,
                             manualCoordinates);
    importer.SetPropertyBool(AI_CONFIG_FBX_CONVERT_TO_M,
                             _sourceCoordinates.metersPerUnit <= 0.0f);

    unsigned int flags = aiProcess_Triangulate
                       | aiProcess_JoinIdenticalVertices
                       | aiProcess_LimitBoneWeights;
    // Direct parser tests also use OBJ fixtures. Preserve their legacy Assimp
    // conversion; the explicit Auto wrapper resolution is FBX-specific.
    if (!explicitCoordinates) {
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

    SourceCoordinatePolicy effectiveCoordinates = _sourceCoordinates;
    if (autoFbxCoordinates) {
        aiMatrix4x4 referenceTransform;
        const bool hasReference = findAutoReferenceTransform(
            scene, referenceTransform);
        bool inferred = false;
        effectiveCoordinates = detail::resolveFbxAutoCoordinatePolicy(
            _sourceCoordinates,
            hasReference ? toAyMatrix(referenceTransform)
                         : ayt::math::Float4x4::identity(),
            &inferred);
        std::fprintf(stderr,
                     "[FBXParser] Auto FBX basis %s up=%u forward=%u "
                     "source=RH target=LH/Y-up/+Z-forward\n",
                     inferred ? "inferred" : "fallback",
                     static_cast<unsigned>(effectiveCoordinates.up),
                     static_cast<unsigned>(effectiveCoordinates.forward));
    }

    std::fprintf(stderr,
                 "[FBXParser] UV contract source=%s target=top-left flipV=%d\n",
                 _sourceCoordinates.uvOrigin == ImportUvOrigin::BottomLeft
                     ? "bottom-left" : "top-left",
                 _sourceCoordinates.uvOrigin == ImportUvOrigin::BottomLeft
                     ? 1 : 0);

    _result = std::make_unique<IntermediateAsset>();

    // Animation source files are a separate asset role. Do not cook their
    // render geometry/materials (Blender and MMD exporters commonly include
    // helper colliders and mmd_edge shells in animation FBX files). Animation
    // channels are name-bound to the model skeleton imported separately.
    if (_loadOption == IConverter::LoadOption::AnimationOnly) {
        // Animation hierarchy baking needs the source deform-bone set even
        // though AnimationOnly deliberately does not emit a Skeleton asset.
        // This lets non-bone FBX container transforms be folded into the
        // nearest runtime bone tracks instead of being silently discarded.
        _prepareSkeletonMapping(scene);
        _parseAnimations(scene);
        if (explicitCoordinates
            && !_applySourceCoordinatePolicy(effectiveCoordinates)) {
            return false;
        }
        return !_result->animations.empty();
    }

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

    if (explicitCoordinates
        && !_applySourceCoordinatePolicy(effectiveCoordinates)) {
        return false;
    }

    // Assimp auto conversion and the explicit manual basis path must both
    // produce the same persisted LH/CW-front contract. This diagnostic is
    // deliberately non-mutating: basis determinant is authoritative, while
    // normals may be authored inconsistently or intentionally discontinuous.
    auditImportedMeshWinding(*_result);

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

bool FBXParser::_applySourceCoordinatePolicy(
    const SourceCoordinatePolicy& policy)
{
    if (!_result) return false;
    if (!std::isfinite(policy.metersPerUnit)
        || policy.metersPerUnit < 0.0f) {
        std::fprintf(stderr, "[FBXParser] invalid metersPerUnit %.9g\n",
                     policy.metersPerUnit);
        return false;
    }

    const ayt::math::FVector3 up = axisVector(policy.up);
    const ayt::math::FVector3 forward = axisVector(policy.forward);
    if (std::abs(up.dot(forward)) > 0.5f) {
        std::fprintf(stderr,
                     "[FBXParser] manual Up and Forward axes must be orthogonal\n");
        return false;
    }

    ayt::math::FVector3 right = up.cross(forward);
    if (policy.handedness == ImportHandedness::Right) {
        right = -right;
    }
    const float determinant = basisDeterminant(right, up, forward);
    const float unitScale = policy.metersPerUnit > 0.0f
        ? policy.metersPerUnit : 1.0f;
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
            reverseTriangleWinding(mesh.indices);
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
                 sourceCoordinatePolicyCacheTag(policy).c_str(),
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
        mesh.skinVertices.resize(totalVertexCount);
        mesh.skinIndexSpace = SkinIndexSpace::GlobalSkeleton;
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
                        if (mesh.skinVertices[vertexIndex].weight[s] < 0.001f) {
                            slot = s;
                            break;
                        }
                    }

                    if (slot == 4) continue;
                    // Store the scene-wide SkeletonData palette index.
                    mesh.skinVertices[vertexIndex].joint[slot] = globalBone->second;
                    mesh.skinVertices[vertexIndex].weight[slot] = vw.mWeight;
                }
            }

            // 归一化权重（确保总和为1）
            for (UInt32 v = 0; v < meshVertexCount; v++) {
                float totalWeight = 0.0f;
                for (UInt32 s = 0; s < 4; s++) {
                    totalWeight += mesh.skinVertices[vertexOffset + v].weight[s];
                }
                if (totalWeight > 0.001f) {
                    for (UInt32 s = 0; s < 4; s++) {
                        mesh.skinVertices[vertexOffset + v].weight[s] /= totalWeight;
                    }
                } else {
                    mesh.skinVertices[vertexOffset + v].joint[0] = 0u;
                    mesh.skinVertices[vertexOffset + v].weight[0] = 1.0f;
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
            mesh.skinVertices.resize(totalVertexCount);
            mesh.skinIndexSpace = SkinIndexSpace::GlobalSkeleton;
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
                            if (mesh.skinVertices[vertexIndex].weight[s] < 0.001f) {
                                slot = s;
                                break;
                            }
                        }
                        if (slot == 4) continue;
                        mesh.skinVertices[vertexIndex].joint[slot] = globalBone->second;
                        mesh.skinVertices[vertexIndex].weight[slot] = vw.mWeight;
                    }
                }

                // 归一化权重
                for (UInt32 v = 0; v < meshVertexCount; v++) {
                    float totalWeight = 0.0f;
                    for (UInt32 s = 0; s < 4; s++) {
                        totalWeight += mesh.skinVertices[vertexOffset + v].weight[s];
                    }
                    if (totalWeight > 0.001f) {
                        for (UInt32 s = 0; s < 4; s++) {
                            mesh.skinVertices[vertexOffset + v].weight[s] /= totalWeight;
                        }
                    } else {
                        mesh.skinVertices[vertexOffset + v].joint[0] = 0u;
                        mesh.skinVertices[vertexOffset + v].weight[0] = 1.0f;
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
    // Keep every Assimp texture semantic and layer. The first binding for a
    // semantic owns the canonical shader slot; further bindings are retained
    // under LayerN names together with their source composition metadata.
    static const std::pair<aiTextureType, const char*> textureTypes[] = {
        {aiTextureType_BASE_COLOR, "baseColorTexture"},
        {aiTextureType_DIFFUSE, "baseColorTexture"},
        {aiTextureType_NORMAL_CAMERA, "normalTexture"},
        {aiTextureType_NORMALS, "normalTexture"},
        {aiTextureType_SPECULAR, "specularTexture"},
        {aiTextureType_EMISSION_COLOR, "emissiveTexture"},
        {aiTextureType_EMISSIVE, "emissiveTexture"},
        {aiTextureType_HEIGHT, "heightTexture"},
        {aiTextureType_SHININESS, "shininessTexture"},
        {aiTextureType_OPACITY, "opacityTexture"},
        {aiTextureType_DISPLACEMENT, "displacementTexture"},
        {aiTextureType_LIGHTMAP, "lightmapTexture"},
        {aiTextureType_METALNESS, "metallicTexture"},
        {aiTextureType_DIFFUSE_ROUGHNESS, "roughnessTexture"},
        {aiTextureType_AMBIENT_OCCLUSION, "aoTexture"},
        {aiTextureType_REFLECTION, "reflectionTexture"},
        {aiTextureType_SHEEN, "sheenTexture"},
        {aiTextureType_CLEARCOAT, "clearcoatTexture"},
        {aiTextureType_TRANSMISSION, "transmissionTexture"},
        {aiTextureType_MAYA_BASE, "mayaBaseTexture"},
        {aiTextureType_MAYA_SPECULAR, "mayaSpecularTexture"},
        {aiTextureType_MAYA_SPECULAR_COLOR, "mayaSpecularColorTexture"},
        {aiTextureType_MAYA_SPECULAR_ROUGHNESS, "mayaSpecularRoughnessTexture"},
        {aiTextureType_ANISOTROPY, "anisotropyTexture"},
        {aiTextureType_GLTF_METALLIC_ROUGHNESS, "metallicRoughnessTexture"},
        {aiTextureType_AMBIENT, "ambientTexture"},
        {aiTextureType_UNKNOWN, "unknownTexture"},
    };

    std::unordered_map<std::string, UInt32> semanticLayerCounts;
    for (const auto& [texType, paramName] : textureTypes) {
        const unsigned int texCount = mat->GetTextureCount(texType);
        for (unsigned int textureIndex = 0; textureIndex < texCount;
             ++textureIndex) {
            aiString texPath;
            aiTextureMapping mapping = aiTextureMapping_UV;
            unsigned int uvIndex = 0;
            ai_real blend = 1.0f;
            aiTextureOp operation = aiTextureOp_Multiply;
            aiTextureMapMode mapModes[3] = {
                aiTextureMapMode_Wrap,
                aiTextureMapMode_Wrap,
                aiTextureMapMode_Wrap};
            if (mat->GetTexture(texType, textureIndex, &texPath, &mapping,
                                &uvIndex, &blend, &operation,
                                mapModes) != AI_SUCCESS) {
                continue;
            }
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

            // PBR and legacy aliases often repeat the exact same binding.
            // Preserve genuinely distinct layers while suppressing aliases
            // that would sample and combine the same source twice.
            const bool duplicateBinding = std::any_of(
                material.textureSources.begin(), material.textureSources.end(),
                [&path, paramName](const MaterialData::TextureSource& source) {
                    const std::string_view existing = source.parameterName;
                    return existing.starts_with(paramName)
                        && sameMaterialTextureSource(source.sourcePath, path);
                });
            if (duplicateBinding) {
                continue;
            }

            const UInt32 semanticLayer = semanticLayerCounts[paramName]++;
            const std::string bindingName = semanticLayer == 0
                ? std::string(paramName)
                : std::string(paramName) + "Layer" + std::to_string(semanticLayer);

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
            param.name = bindingName;
            param.type = MaterialParamType::Texture2D;
            param.texturePath = makeTextureVirtualPath(textureName, contract.usageSuffix,
                                                       texExt.c_str());
            material.parameters.push_back(param);

            material.texturePaths.push_back(path);
            MaterialData::TextureSource source;
            source.parameterName = bindingName;
            source.sourcePath = path;
            source.virtualPath = param.texturePath;
            source.usageSuffix = contract.usageSuffix;
            source.colorSpace = contract.colorSpace;
            source.normalY = contract.normalY;
            source.layerIndex = semanticLayer;
            source.sourceSemantic = static_cast<UInt32>(texType);
            source.sourceLayer = textureIndex;
            source.uvChannel = uvIndex;
            source.mapping = static_cast<MaterialTextureMapping>(mapping);
            source.operation = static_cast<MaterialTextureOperation>(operation);
            source.wrapU = static_cast<MaterialTextureWrap>(mapModes[0]);
            source.wrapV = static_cast<MaterialTextureWrap>(mapModes[1]);
            source.wrapW = static_cast<MaterialTextureWrap>(mapModes[2]);
            source.blendFactor = blend;

            aiUVTransform transform;
            if (mat->Get(AI_MATKEY_UVTRANSFORM(texType, textureIndex),
                         transform) == AI_SUCCESS) {
                source.hasUvTransform = true;
                source.uvTranslation[0] = transform.mTranslation.x;
                source.uvTranslation[1] = transform.mTranslation.y;
                source.uvScale[0] = transform.mScaling.x;
                source.uvScale[1] = transform.mScaling.y;
                source.uvRotation = transform.mRotation;
            }
            unsigned int textureFlags = 0;
            if (mat->Get(AI_MATKEY_TEXFLAGS(texType, textureIndex),
                         textureFlags) == AI_SUCCESS) {
                source.flags = textureFlags;
            }
            material.textureSources.push_back(std::move(source));

        }
    }
}

void FBXParser::_parseMaterial(const void* aiMatPtr, size_t index) {
    const aiMaterial* mat = static_cast<const aiMaterial*>(aiMatPtr);

    MaterialData material;
    captureSourceProperties(mat, material);
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

    // Import surface metadata into the source-neutral material contract.
    // Assimp's blend property describes the composition formula, not whether
    // the surface is transparent. In particular aiBlendMode_Default means
    // standard alpha compositing and must not by itself force the Blend route.
    float importedOpacity = 1.0f;
    const bool hasOpacity =
        mat->Get(AI_MATKEY_OPACITY, importedOpacity) == AI_SUCCESS;
    float importedTransparency = 0.0f;
    const bool hasTransparency =
        mat->Get(AI_MATKEY_TRANSPARENCYFACTOR,
                 importedTransparency) == AI_SUCCESS;
    aiBlendMode importedBlend = aiBlendMode_Default;
    const bool hasBlend =
        mat->Get(AI_MATKEY_BLEND_FUNC, importedBlend) == AI_SUCCESS;
    int importedTwoSided = 0;
    (void)mat->Get(AI_MATKEY_TWOSIDED, importedTwoSided);

    importedOpacity = std::clamp(importedOpacity, 0.0f, 1.0f);
    importedTransparency = std::clamp(importedTransparency, 0.0f, 1.0f);
    if (!hasOpacity && hasTransparency) {
        importedOpacity = 1.0f - importedTransparency;
    }
    material.blendFunction = hasBlend
        && importedBlend == aiBlendMode_Additive
        ? MaterialBlendFunction::Additive
        : MaterialBlendFunction::StandardAlpha;
    const bool explicitBlend = importedOpacity < 0.999f
        || (hasTransparency && importedTransparency > 0.001f)
        || material.blendFunction == MaterialBlendFunction::Additive;
    material.alphaMode = explicitBlend
        ? MaterialAlphaMode::Blend : MaterialAlphaMode::Opaque;
    material.alphaCutoff = 0.5f;
    material.doubleSided = importedTwoSided != 0;
    material.surfaceSource = (explicitBlend || material.doubleSided)
        ? MaterialSurfaceSource::ExplicitSource
        : MaterialSurfaceSource::Default;

    aiShadingMode importedShading = aiShadingMode_Phong;
    if (mat->Get(AI_MATKEY_SHADING_MODEL, importedShading) == AI_SUCCESS) {
        material.shadingModel = translateShadingModel(importedShading);
    }

    // baseColor (albedo). PBR base color is authoritative; diffuse remains a
    // compatibility fallback for legacy FBX/Phong exporters.
    aiColor4D baseColor;
    baseColor.r = 1.0f;
    baseColor.g = 1.0f;
    baseColor.b = 1.0f;
    baseColor.a = 1.0f;
    bool hasBaseColor = false;
#ifdef AI_MATKEY_BASE_COLOR
    hasBaseColor = mat->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS;
#endif
    if (!hasBaseColor) {
        (void)mat->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor);
    }
    if (!hasOpacity && !hasTransparency && baseColor.a < 0.999f) {
        importedOpacity = std::clamp(baseColor.a, 0.0f, 1.0f);
        material.alphaMode = MaterialAlphaMode::Blend;
        material.surfaceSource = MaterialSurfaceSource::ExplicitSource;
    }
    Param baseColorParam;
    baseColorParam.name = "baseColor";
    baseColorParam.type = MaterialParamType::Float4;
    baseColorParam.float4Value[0] = baseColor.r;
    baseColorParam.float4Value[1] = baseColor.g;
    baseColorParam.float4Value[2] = baseColor.b;
    baseColorParam.float4Value[3] = baseColor.a;
    material.parameters.push_back(baseColorParam);

    if (hasTransparency) {
        setFloatParam(material, "transparencyFactor", importedTransparency);
    }

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
    metallic = std::clamp(metallic, 0.0f, 1.0f);
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
    float glossiness = 0.0f;
    const bool hasGlossiness =
        mat->Get(AI_MATKEY_GLOSSINESS_FACTOR, glossiness) == AI_SUCCESS;
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
    if (!hasPbrRoughness && !parsedStringRoughness) {
        if (hasGlossiness) {
            roughness = 1.0f - std::clamp(glossiness, 0.0f, 1.0f);
        } else if (hasLegacyShininess) {
            roughness = std::sqrt(
                2.0f / (std::max(0.0f, legacyShininess) + 2.0f));
        }
    }
    roughness = std::clamp(roughness, 0.045f, 1.0f);
    Param roughnessParam;
    roughnessParam.name = "roughness";
    roughnessParam.type = MaterialParamType::Float;
    roughnessParam.floatValue = roughness;
    material.parameters.push_back(roughnessParam);

    if (hasGlossiness) {
        setFloatParam(material, "glossiness",
                      std::clamp(glossiness, 0.0f, 1.0f));
    }

    Param aoParam;
    aoParam.name = "ao";
    aoParam.type = MaterialParamType::Float;
    aoParam.floatValue = 1.0f;
    material.parameters.push_back(aoParam);

    aiColor3D ambientColor;
    if (mat->Get(AI_MATKEY_COLOR_AMBIENT, ambientColor) == AI_SUCCESS) {
        setFloat3Param(material, "ambientColor",
                       ambientColor.r, ambientColor.g, ambientColor.b);
    }

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
    const float opacity = importedOpacity;
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

    // Preserve the rest of Assimp's canonical material model. The bundled
    // PBR shader may not consume every value yet, but .aymat retains them for
    // custom shaders, tooling, and a future FBX-SDK source adapter.
    appendOptionalFloat(mat, material, "bumpScaling", AI_MATKEY_BUMPSCALING);
    appendOptionalFloat(mat, material, "reflectivity", AI_MATKEY_REFLECTIVITY);
    appendOptionalFloat(mat, material, "shininessStrength",
                        AI_MATKEY_SHININESS_STRENGTH);
    appendOptionalFloat(mat, material, "ior", AI_MATKEY_REFRACTI);
    appendOptionalFloat(mat, material, "specularFactor",
                        AI_MATKEY_SPECULAR_FACTOR);
    appendOptionalFloat(mat, material, "anisotropy",
                        AI_MATKEY_ANISOTROPY_FACTOR);
    appendOptionalFloat(mat, material, "anisotropyRotation",
                        AI_MATKEY_ANISOTROPY_ROTATION);
    appendOptionalFloat(mat, material, "sheenRoughness",
                        AI_MATKEY_SHEEN_ROUGHNESS_FACTOR);
    appendOptionalFloat(mat, material, "clearcoat",
                        AI_MATKEY_CLEARCOAT_FACTOR);
    appendOptionalFloat(mat, material, "clearcoatRoughness",
                        AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR);
    appendOptionalFloat(mat, material, "volumeThickness",
                        AI_MATKEY_VOLUME_THICKNESS_FACTOR);
    appendOptionalFloat(mat, material, "attenuationDistance",
                        AI_MATKEY_VOLUME_ATTENUATION_DISTANCE);
    appendOptionalFloat(mat, material, "emissiveIntensity",
                        AI_MATKEY_EMISSIVE_INTENSITY);
    appendOptionalColor3(mat, material, "transparentColor",
                         AI_MATKEY_COLOR_TRANSPARENT);
    appendOptionalColor3(mat, material, "reflectiveColor",
                         AI_MATKEY_COLOR_REFLECTIVE);
    appendOptionalColor3(mat, material, "sheenColor",
                         AI_MATKEY_SHEEN_COLOR_FACTOR);
    appendOptionalColor3(mat, material, "attenuationColor",
                         AI_MATKEY_VOLUME_ATTENUATION_COLOR);

    float transmission = 0.0f;
    if (mat->Get(AI_MATKEY_TRANSMISSION_FACTOR, transmission) == AI_SUCCESS) {
        transmission = std::clamp(transmission, 0.0f, 1.0f);
        setFloatParam(material, "transmission", transmission);
        if (transmission > 0.001f) {
            material.alphaMode = MaterialAlphaMode::Blend;
            material.surfaceSource = MaterialSurfaceSource::ExplicitSource;
        }
    }

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
    _collectSkeletonBones(scene->mRootNode, -1, _boneNodeNames, skeleton);

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
        if (mesh.skinVertices.empty()) continue;
        const std::size_t vertexCount = mesh.positions.size() / 3u;
        if (boneCount == 0 || mesh.skinVertices.size() != vertexCount
            || mesh.skinIndexSpace != SkinIndexSpace::GlobalSkeleton) {
            std::fprintf(stderr,
                         "[FBXParser] mesh '%s' has invalid global skin payload "
                         "(vertices=%zu skinVertices=%zu bones=%zu)\n",
                         mesh.name.c_str(), vertexCount,
                         mesh.skinVertices.size(), boneCount);
            return false;
        }
        for (std::size_t v = 0; v < vertexCount; ++v) {
            float sum = 0.0f;
            for (std::size_t slot = 0; slot < 4; ++slot) {
                const UInt32 index = mesh.skinVertices[v].joint[slot];
                const float weight = mesh.skinVertices[v].weight[slot];
                if (weight > 0.0f && index >= boneCount) {
                    std::fprintf(stderr,
                                 "[FBXParser] mesh '%s' vertex %zu references "
                                 "invalid bone %u/%zu\n",
                                 mesh.name.c_str(), v, index, boneCount);
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
                                      SkeletonData& skeleton) {
    if (!node) return;

    std::string nodeName = node->mName.C_Str();
    const bool isBone = boneNodeNames.find(nodeName) != boneNodeNames.end();

    int thisIndex = -1;
    if (isBone) {
        BoneData bone;
        bone.name = nodeName;
        bone.parentIndex = parentIndex;

        // aiBone::mOffsetMatrix is mesh-bind -> bone-bind.  It is the only
        // authoritative bind-space value shared by all skinned submeshes.
        // FBX node hierarchies may additionally contain Blender unit/object
        // wrappers (for example a 100x armature and mesh node) even when the
        // vertices and offsets are already expressed in metres.  Folding such
        // wrappers into the skeleton scales skin matrices a second time.
        const auto offset = _boneOffsets.find(nodeName);
        if (offset != _boneOffsets.end()) {
            bone.inverseBindMatrix = offset->second;
        } else {
            bone.inverseBindMatrix = ayt::math::Float4x4::identity();
            std::fprintf(stderr,
                         "[FBXParser] bone '%s' has no skin-cluster offset; "
                         "using identity bind transform\n",
                         nodeName.c_str());
        }

        // Reconstruct a canonical local rest pose from bind worlds.  For a
        // child C of parent P: local(C) = inverse(bindWorld(P))*bindWorld(C).
        // Runtime accumulation therefore guarantees
        // bindWorld(C)*inverseBind(C) == identity for every palette entry,
        // independent of DCC helper nodes and object-level scale.
        const ayt::math::Float4x4 bindWorld = bone.inverseBindMatrix.inverse();
        ayt::math::Float4x4 local = bindWorld;
        if (parentIndex >= 0
            && static_cast<size_t>(parentIndex) < skeleton.bones.size()) {
            local = skeleton.bones[static_cast<size_t>(parentIndex)].inverseBindMatrix
                  * bindWorld;
        }
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

    // aiNode only supplies topology here.  Bind matrices supply transforms.
    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        _collectSkeletonBones(node->mChildren[i],
                              isBone ? thisIndex : parentIndex,
                              boneNodeNames, skeleton);
    }
}

namespace {

aiVector3D sampleVectorKeys(const aiVectorKey* keys, unsigned int count,
                            double tick, const aiVector3D& fallback)
{
    if (keys == nullptr || count == 0u) return fallback;
    if (tick <= keys[0].mTime) return keys[0].mValue;
    if (tick >= keys[count - 1u].mTime) return keys[count - 1u].mValue;
    unsigned int high = 1u;
    while (high < count && keys[high].mTime < tick) ++high;
    const unsigned int low = high - 1u;
    const double span = keys[high].mTime - keys[low].mTime;
    const float alpha = span > 0.0
        ? static_cast<float>((tick - keys[low].mTime) / span) : 0.0f;
    return keys[low].mValue + (keys[high].mValue - keys[low].mValue) * alpha;
}

aiQuaternion sampleQuaternionKeys(const aiQuatKey* keys, unsigned int count,
                                  double tick, const aiQuaternion& fallback)
{
    if (keys == nullptr || count == 0u) return fallback;
    if (tick <= keys[0].mTime) return keys[0].mValue;
    if (tick >= keys[count - 1u].mTime) return keys[count - 1u].mValue;
    unsigned int high = 1u;
    while (high < count && keys[high].mTime < tick) ++high;
    const unsigned int low = high - 1u;
    const double span = keys[high].mTime - keys[low].mTime;
    const float alpha = span > 0.0
        ? static_cast<float>((tick - keys[low].mTime) / span) : 0.0f;
    aiQuaternion result;
    aiQuaternion::Interpolate(result, keys[low].mValue, keys[high].mValue, alpha);
    result.Normalize();
    return result;
}

ayt::math::Float4x4 sampleNodeLocal(const aiNode* node,
                                    const aiNodeAnim* channel,
                                    double tick)
{
    if (channel == nullptr) return toAyMatrix(node->mTransformation);
    aiVector3D restScale;
    aiVector3D restPosition;
    aiQuaternion restRotation;
    node->mTransformation.Decompose(restScale, restRotation, restPosition);
    const aiVector3D scale = sampleVectorKeys(
        channel->mScalingKeys, channel->mNumScalingKeys, tick, restScale);
    const aiVector3D position = sampleVectorKeys(
        channel->mPositionKeys, channel->mNumPositionKeys, tick, restPosition);
    const aiQuaternion rotation = sampleQuaternionKeys(
        channel->mRotationKeys, channel->mNumRotationKeys, tick, restRotation);
    return toAyMatrix(aiMatrix4x4(scale, rotation, position));
}

void collectNodesByName(const aiNode* node,
                        std::unordered_map<std::string, const aiNode*>& nodes)
{
    if (node == nullptr) return;
    nodes.emplace(node->mName.C_Str(), node);
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        collectNodesByName(node->mChildren[i], nodes);
    }
}

const aiNode* findSkinnedMeshNode(const aiNode* node, const aiScene* scene)
{
    if (node == nullptr || scene == nullptr) return nullptr;
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        const unsigned int meshIndex = node->mMeshes[i];
        if (meshIndex < scene->mNumMeshes
            && scene->mMeshes[meshIndex] != nullptr
            && scene->mMeshes[meshIndex]->HasBones()) {
            return node;
        }
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        if (const aiNode* found = findSkinnedMeshNode(node->mChildren[i], scene)) {
            return found;
        }
    }
    return nullptr;
}

std::vector<const aiNode*> pathFromSceneRoot(const aiNode* node)
{
    std::vector<const aiNode*> path;
    for (const aiNode* current = node; current != nullptr; current = current->mParent) {
        path.push_back(current);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

ayt::math::Float4x4 sampleNodePathWorld(
    const std::vector<const aiNode*>& path,
    const std::unordered_map<std::string, const aiNodeAnim*>& channels,
    double tick)
{
    ayt::math::Float4x4 world = ayt::math::Float4x4::identity();
    for (const aiNode* pathNode : path) {
        const auto channel = channels.find(pathNode->mName.C_Str());
        world = world * sampleNodeLocal(
            pathNode, channel == channels.end() ? nullptr : channel->second, tick);
    }
    return world;
}

void appendNodeKeyTimes(
    const aiNode* node,
    const std::unordered_map<std::string, const aiNodeAnim*>& channels,
    std::vector<double>& keyTimes,
    bool& animatedPath)
{
    if (node == nullptr) return;
    const auto channel = channels.find(node->mName.C_Str());
    if (channel == channels.end()) return;
    animatedPath = true;
    const aiNodeAnim* animation = channel->second;
    for (unsigned int k = 0; k < animation->mNumPositionKeys; ++k)
        keyTimes.push_back(animation->mPositionKeys[k].mTime);
    for (unsigned int k = 0; k < animation->mNumRotationKeys; ++k)
        keyTimes.push_back(animation->mRotationKeys[k].mTime);
    for (unsigned int k = 0; k < animation->mNumScalingKeys; ++k)
        keyTimes.push_back(animation->mScalingKeys[k].mTime);
}

void appendDirectNodeTracks(const aiAnimation* anim, AnimationData& data)
{
    for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci) {
        const aiNodeAnim* channel = anim->mChannels[ci];
        if (channel == nullptr) continue;
        const std::string nodeName = channel->mNodeName.C_Str();

        auto appendVectorTrack = [&](const char* property,
                                     const aiVectorKey* keys,
                                     unsigned int count) {
            if (keys == nullptr || count == 0u) return;
            KeyframeTrack track;
            track.targetNode = nodeName;
            track.property = property;
            track.valueType = AnimTrackType::Vector3;
            track.times.reserve(count);
            track.values.reserve(static_cast<size_t>(count) * 3u);
            for (unsigned int k = 0; k < count; ++k) {
                track.times.push_back(static_cast<Float32>(keys[k].mTime));
                track.values.push_back(keys[k].mValue.x);
                track.values.push_back(keys[k].mValue.y);
                track.values.push_back(keys[k].mValue.z);
            }
            data.tracks.push_back(std::move(track));
        };
        appendVectorTrack("position", channel->mPositionKeys,
                          channel->mNumPositionKeys);

        if (channel->mRotationKeys != nullptr && channel->mNumRotationKeys > 0u) {
            KeyframeTrack track;
            track.targetNode = nodeName;
            track.property = "rotation";
            track.valueType = AnimTrackType::Quaternion;
            track.times.reserve(channel->mNumRotationKeys);
            track.values.reserve(static_cast<size_t>(channel->mNumRotationKeys) * 4u);
            for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k) {
                track.times.push_back(static_cast<Float32>(channel->mRotationKeys[k].mTime));
                track.values.push_back(channel->mRotationKeys[k].mValue.x);
                track.values.push_back(channel->mRotationKeys[k].mValue.y);
                track.values.push_back(channel->mRotationKeys[k].mValue.z);
                track.values.push_back(channel->mRotationKeys[k].mValue.w);
            }
            data.tracks.push_back(std::move(track));
        }

        appendVectorTrack("scale", channel->mScalingKeys,
                          channel->mNumScalingKeys);
    }
}

void appendHierarchyBakedBoneTracks(
    const aiScene* scene,
    const aiAnimation* anim,
    const std::unordered_set<std::string>& boneNames,
    const std::unordered_map<std::string, UInt32>& boneNameToIndex,
    AnimationData& data)
{
    std::unordered_map<std::string, const aiNode*> nodes;
    collectNodesByName(scene->mRootNode, nodes);
    std::unordered_map<std::string, const aiNodeAnim*> channels;
    channels.reserve(anim->mNumChannels);
    for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci) {
        if (anim->mChannels[ci] != nullptr) {
            channels.emplace(anim->mChannels[ci]->mNodeName.C_Str(),
                             anim->mChannels[ci]);
        }
    }

    std::vector<std::string> orderedBones(boneNameToIndex.size());
    for (const auto& [name, index] : boneNameToIndex) {
        if (index < orderedBones.size()) orderedBones[index] = name;
    }

    // aiBone offsets and mesh vertices live in mesh bind space.  Root bone
    // tracks must use that same space.  Using scene-root space would retain
    // Blender/FBX armature wrappers (commonly a 90-degree axis rotation and a
    // 100x unit node) and then coordinate conversion would rotate them again.
    const aiNode* skinnedMeshNode = findSkinnedMeshNode(scene->mRootNode, scene);
    if (skinnedMeshNode == nullptr) {
        std::fprintf(stderr,
                     "[FBXParser] animation source has deform bones but no "
                     "skinned mesh node; root tracks use scene space\n");
    }

    UInt32 bakedBoneCount = 0u;
    UInt32 meshSpaceRootCount = 0u;
    for (const std::string& boneName : orderedBones) {
        if (boneName.empty()) continue;
        const auto nodeIt = nodes.find(boneName);
        if (nodeIt == nodes.end()) continue;

        const aiNode* boneNode = nodeIt->second;
        const aiNode* parentBoneNode = boneNode->mParent;
        while (parentBoneNode != nullptr
               && boneNames.find(parentBoneNode->mName.C_Str()) == boneNames.end()) {
            parentBoneNode = parentBoneNode->mParent;
        }
        const std::vector<const aiNode*> boneWorldPath =
            pathFromSceneRoot(boneNode);
        const std::vector<const aiNode*> parentWorldPath =
            pathFromSceneRoot(parentBoneNode);
        const std::vector<const aiNode*> meshWorldPath =
            pathFromSceneRoot(skinnedMeshNode);

        std::vector<double> keyTimes;
        bool animatedPath = false;
        // Child-local motion depends only on the path below its nearest deform
        // parent.  Root-local motion is relative to the skinned mesh node, so
        // include both scene paths in case either wrapper is animated.
        for (const aiNode* current = boneNode;
             current != nullptr && current != parentBoneNode;
             current = current->mParent) {
            appendNodeKeyTimes(current, channels, keyTimes, animatedPath);
        }
        if (parentBoneNode == nullptr) {
            for (const aiNode* current = boneNode->mParent;
                 current != nullptr; current = current->mParent) {
                appendNodeKeyTimes(current, channels, keyTimes, animatedPath);
            }
            for (const aiNode* current = skinnedMeshNode;
                 current != nullptr; current = current->mParent) {
                appendNodeKeyTimes(current, channels, keyTimes, animatedPath);
            }
        }
        if (!animatedPath || keyTimes.empty()) continue;
        std::sort(keyTimes.begin(), keyTimes.end());
        keyTimes.erase(std::unique(keyTimes.begin(), keyTimes.end(),
            [](double a, double b) { return std::abs(a - b) <= 1.0e-9; }),
            keyTimes.end());

        KeyframeTrack positionTrack;
        positionTrack.targetNode = boneName;
        positionTrack.property = "position";
        positionTrack.valueType = AnimTrackType::Vector3;
        KeyframeTrack rotationTrack;
        rotationTrack.targetNode = boneName;
        rotationTrack.property = "rotation";
        rotationTrack.valueType = AnimTrackType::Quaternion;
        KeyframeTrack scaleTrack;
        scaleTrack.targetNode = boneName;
        scaleTrack.property = "scale";
        scaleTrack.valueType = AnimTrackType::Vector3;
        for (KeyframeTrack* track : {&positionTrack, &rotationTrack, &scaleTrack}) {
            track->times.reserve(keyTimes.size());
        }
        positionTrack.values.reserve(keyTimes.size() * 3u);
        rotationTrack.values.reserve(keyTimes.size() * 4u);
        scaleTrack.values.reserve(keyTimes.size() * 3u);

        ayt::math::FQuaternion previousRotation =
            ayt::math::FQuaternion::identity();
        bool hasPreviousRotation = false;
        bool boneValid = true;
        for (double keyTime : keyTimes) {
            const ayt::math::Float4x4 boneWorld =
                sampleNodePathWorld(boneWorldPath, channels, keyTime);
            ayt::math::Float4x4 collapsed = boneWorld;
            if (parentBoneNode != nullptr) {
                collapsed = sampleNodePathWorld(parentWorldPath, channels, keyTime).inverse()
                          * boneWorld;
            } else if (skinnedMeshNode != nullptr) {
                collapsed = sampleNodePathWorld(meshWorldPath, channels, keyTime).inverse()
                          * boneWorld;
            }

            ayt::math::FVector3 position;
            ayt::math::FQuaternion rotation;
            ayt::math::FVector3 scale;
            if (!collapsed.decompose(position, rotation, scale)) {
                std::fprintf(stderr,
                             "[FBXParser] cannot bake animation hierarchy for bone '%s'\n",
                             boneName.c_str());
                boneValid = false;
                break;
            }
            rotation = rotation.normalize();
            if (hasPreviousRotation
                && previousRotation.x * rotation.x
                 + previousRotation.y * rotation.y
                 + previousRotation.z * rotation.z
                 + previousRotation.w * rotation.w < 0.0f) {
                rotation.x = -rotation.x;
                rotation.y = -rotation.y;
                rotation.z = -rotation.z;
                rotation.w = -rotation.w;
            }
            previousRotation = rotation;
            hasPreviousRotation = true;

            const Float32 time = static_cast<Float32>(keyTime);
            positionTrack.times.push_back(time);
            rotationTrack.times.push_back(time);
            scaleTrack.times.push_back(time);
            positionTrack.values.insert(positionTrack.values.end(),
                {position.x, position.y, position.z});
            rotationTrack.values.insert(rotationTrack.values.end(),
                {rotation.x, rotation.y, rotation.z, rotation.w});
            scaleTrack.values.insert(scaleTrack.values.end(),
                {scale.x, scale.y, scale.z});
        }
        if (!boneValid) continue;
        data.tracks.push_back(std::move(positionTrack));
        data.tracks.push_back(std::move(rotationTrack));
        data.tracks.push_back(std::move(scaleTrack));
        if (parentBoneNode == nullptr && skinnedMeshNode != nullptr) {
            ++meshSpaceRootCount;
        }
        ++bakedBoneCount;
    }
    std::fprintf(stderr,
                 "[FBXParser] baked animation hierarchy bones=%u "
                 "meshSpaceRoots=%u referenceMesh='%s'\n",
                 bakedBoneCount, meshSpaceRootCount,
                 skinnedMeshNode != nullptr ? skinnedMeshNode->mName.C_Str() : "(none)");
}

} // namespace

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
        // mTicksPerSecond == 0 在 Assimp 契约里表示 "use scene default",fallback 30
        data.ticksPerSecond = anim->mTicksPerSecond != 0.0
            ? static_cast<Float32>(anim->mTicksPerSecond)
            : 30.0f;

        const double ticks = (anim->mTicksPerSecond != 0.0)
            ? anim->mTicksPerSecond
            : 30.0;
        // IAnimation keeps clip duration in seconds while track key times stay
        // in source ticks. AnimationPlayer normalizes the latter exactly once
        // when a clip is bound. Converting keys here as well compresses the
        // sampled motion by ticksPerSecond a second time.
        data.duration = static_cast<Float32>(anim->mDuration / ticks);

        if (!_boneNodeNames.empty() && !_boneNameToIndex.empty()) {
            appendHierarchyBakedBoneTracks(
                scene, anim, _boneNodeNames, _boneNameToIndex, data);
        } else {
            // Meshless animation sources do not expose a deform-bone set via
            // Assimp. Preserve their channels verbatim; callers that target a
            // separate skeleton can still bind by name, but no hierarchy
            // compaction can be proven without target-skeleton metadata.
            appendDirectNodeTracks(anim, data);
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
                    track.times.push_back(static_cast<Float32>(key.mTime));
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
