#pragma once
#include "AYResource/IntermediateAsset.h"
#include "AYResource/IConverter.h"
#include <string>
#include <memory>
#include <unordered_set>

// 前向声明
struct aiScene;
struct aiNode;
struct aiMesh;
struct aiMaterial;
struct aiAnimation;
struct aiNodeAnim;

namespace ayt::resource
{

// ===== FBXParser — 使用 Assimp 解析 FBX =====
// 输出 IntermediateAsset，供 MeshConverter/MaterialConverter 等使用
class FBXParser : public IFormatParser {
public:
    FBXParser() = default;
    explicit FBXParser(const std::string& sourcePath);

    // ===== IFormatParser =====
    bool parse(const std::string& sourcePath) override;
    std::unique_ptr<IntermediateAsset> getResult() override;
    const char* getFormatName() const override { return "FBX"; }

    // ===== 配置 =====
    void setLoadOption(IConverter::LoadOption option) { _loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return _loadOption; }

    /// @brief 设置是否按 FBX 节点分离模型（每个节点一个 MeshData）
    /// @param separate true=每个节点一个MeshData，false=合并所有到一个MeshData
    void setSeparateModels(bool separate) { _separateModels = separate; }
    bool getSeparateModels() const { return _separateModels; }

    /// Asset base name used in virtual material/texture paths
    /// (e.g. "Sour" → materials/Sour_material_0.aymat). Must be set
    /// before parse() so mesh.materialSlots match MaterialConverter output.
    void setAssetBaseName(const std::string& baseName) { _assetBaseName = baseName; }
    const std::string& getAssetBaseName() const { return _assetBaseName; }

    /// Usage suffix for cooked texture virtual paths (default "_d").
    void setTextureUsageSuffix(const std::string& suffix) { _textureUsageSuffix = suffix; }
    const std::string& getTextureUsageSuffix() const { return _textureUsageSuffix; }

    /// Dev raw-reference mode (ImportOptions.cookTextures=false): texture
    /// params reference the source extension (textures/foo_d.png) instead
    /// of .aytex. dds/aytex sources are excluded — they still flow through
    /// the passthrough → .aytex cook path (zero decode).
    void setPreserveSourceExtension(bool preserve) { _preserveSourceExtension = preserve; }
    bool getPreserveSourceExtension() const { return _preserveSourceExtension; }

private:
    std::string _sourcePath;
    std::string _assetBaseName;
    std::string _textureUsageSuffix = "_d";
    bool _preserveSourceExtension = false;
    IConverter::LoadOption _loadOption = IConverter::LoadOption::Full;  // 默认 Full 模式
    bool _separateModels = true;  // 默认分离，每个 aiMesh 一个 MeshData
    std::unique_ptr<IntermediateAsset> _result;

    // 解析辅助
    void _parseMesh(const void* aiMesh, size_t index, const std::string& uniqueName);
    void _parseMaterial(const void* aiMaterial, size_t index);
    void _extractMaterialTextures(const struct aiMaterial* mat, struct MaterialData& material);
    void _parseTexture(const void* aiTexture, size_t index);
    void _parseAllMeshesAsOne(const aiScene* scene);
    void _collectNodeMeshes(const aiNode* node, const aiScene* scene, const std::string& parentPath);
    void _parseSkeletons(const aiScene* scene);
    void _collectSkeletonBones(const aiNode* node, int parentIndex,
                               const std::unordered_set<std::string>& boneNodeNames,
                               SkeletonData& skeleton);
    // R-02: 把 aiNodeAnim 的 channel 转换为 3 条 KeyframeTrack (position/rotation/scale)
    // valueType 由 property 推断 (rotation → Quaternion, 其它 → Vector3)
    void _parseAnimations(const aiScene* scene);
    UInt8 _getMeshAttributeMask(const aiMesh* m);
};

} // namespace ayt::resource