#pragma once
#include "IAYMesh.h"
#include <vector>
#include <string>
#include <array>
#include <AYMathTypes.h>
#include <AYFile.h>

namespace ayt::resource
{

// ===== Mesh 文件头 (二进制格式) =====
#pragma pack(push, 1)
struct MeshBinaryHeader {
    UInt32 magic;              // 'AYMH' = 0x484D5941
    UInt16 version;            // 版本 = 1
    FGuid guid;                // 资源唯一标识 (16 bytes)
    UInt8  attributeMask;      // MeshAttribute 位掩码
    UInt8  flags;               // 标志 (bit 0: hasSkinWeights)
    UInt32 vertexCount;        // 顶点数量
    UInt32 indexCount;          // 索引数量
    UInt32 submeshCount;        // Submesh 数量
    UInt32 materialSlotCount;   // Material slot 数量
    Float32 boundsCenter[3];    // 包围盒中心
    Float32 boundsHalfExtent[3]; // 包围盒半尺寸
    UInt8  hasBounds;          // 是否有预计算包围盒
    UInt8  hasSkinWeights;      // 是否有骨骼权重
    UInt8  padding[2];          // 对齐填充 (调整后共48 bytes)
};
#pragma pack(pop)

// ===== Mesh — IMesh 实现类 =====
class Mesh : public IMesh {
public:
    Mesh();
    virtual ~Mesh() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    // ===== IMesh =====
    UInt32 getVertexCount() const override { return _vertexCount; }
    UInt32 getVertexStride() const override { return _vertexStride; }
    const UInt8* getVertexData() const override { return _vertexData.data(); }

    UInt32 getIndexCount() const override { return _indexCount; }
    const UInt32* getIndexData() const override { return _indices.data(); }

    UInt8 getAttributeMask() const override { return _attributeMask; }
    AttributeInfo getAttributeInfo(MeshAttribute attr) const override {
        return _attrInfo[static_cast<size_t>(attr)];
    }

    UInt32 getSubmeshCount() const override { return static_cast<UInt32>(_submeshes.size()); }
    const Submesh* getSubmeshes() const override { return _submeshes.data(); }

    UInt32 getMaterialSlotCount() const override { return static_cast<UInt32>(_materialSlots.size()); }
    const char* getMaterialSlot(UInt32 index) const override { return _materialSlots[index].c_str(); }

    // ===== Bounds =====
    Bounds getBounds() const override { return _bounds; }
    void getBounds(ayt::math::FVector3& min, ayt::math::FVector3& max) const override {
        min = _bounds.getMin();
        max = _bounds.getMax();
    }
    Bool hasBounds() const override { return _hasBounds; }

    // ===== Skin weights =====
    Bool hasSkinWeights() const override { return _hasSkinWeights; }
    const VertexSkinWeight* getSkinWeights() const override {
        return _skinWeights.empty() ? nullptr : _skinWeights.data();
    }

    // ===== LOD =====
    UInt32 getLODCount() const override { return static_cast<UInt32>(_lodData.size()); }
    const LODData* getLODData() const override { return _lodData.data(); }
    IMesh* getLOD(UInt32 lodIndex) override { return _lods[lodIndex]; }

    // ===== Extensions =====
    UInt32 getExtensionCount() const override { return static_cast<UInt32>(_extensions.size()); }
    const Extension* getExtension(UInt32 index) const override { return &_extensions[index]; }
    const Extension* findExtension(UInt32 type) const override;

    // ===== 从二进制加载/保存 =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== 创建测试数据 =====
    void createCube(Float32 size = 1.0f);
    void createSphere(Float32 radius = 1.0f, UInt32 segments = 16);

private:
    void _computeBounds();
    void _computeVertexStride();
    void _clear();

    FGuid _guid;  // 资源唯一标识

    // ===== 顶点数据 (交错布局) =====
    UInt32 _vertexCount = 0;
    UInt32 _vertexStride = 0;
    std::vector<UInt8> _vertexData;

    // ===== 属性信息 =====
    UInt8 _attributeMask = 0;
    std::array<AttributeInfo, 6> _attrInfo{};  // 6 for SkinWeight

    // ===== 索引数据 =====
    UInt32 _indexCount = 0;
    std::vector<UInt32> _indices;

    // ===== Submesh + Material =====
    std::vector<Submesh> _submeshes;
    std::vector<std::string> _materialSlots;

    // ===== Bounds (预计算) =====
    Bool _hasBounds = false;
    Bounds _bounds;

    // ===== Skin weights =====
    Bool _hasSkinWeights = false;
    std::vector<VertexSkinWeight> _skinWeights;

    // ===== LOD (预留扩展) =====
    std::vector<LODData> _lodData;
    std::vector<Mesh*> _lods;

    // ===== Extensions (预留扩展槽) =====
    std::vector<Extension> _extensions;

    // ===== 路径 =====
    std::string _path;

    // ===== 友元 =====
    friend class FBXConverter;
    friend class GLTFConverter;
};

} // namespace ayt::resource