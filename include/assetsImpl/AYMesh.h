#pragma once
#include "IAYMesh.h"
#include <vector>
#include <string>
#include <array>
#include <AYMathTypes.h>
#include <AYFile.h>

namespace ayt::resource
{

// ===== Mesh 文件头 (二进制格式 v1-chunked) =====
//
// 文件 layout:
//   [MeshBinaryHeader] [ChunkDirectory][N × 12 bytes] [ChunkData...]
//
// Chunk-directory entry 顺序可任意；load 时按 four-cc 查表加载。
// 多通道 (pos/norm/uv/tangent/color) 不再直接拼成 interleaved bytes 写进文件，
// 而是以独立 chunk 存放，由 loader 在内存中重新交错。
//
// Flags:
//   bit 0: hasBounds
//   bit 1: hasSkinWeights      (与 attributeMask 的 SkinWeight 位冗余；保留以便快速 refuse)
//
// attributeMask (8-bit) 与原版兼容——位 5 (SkinWeight) 与 chunk 'SKIN' 同时表示。
//
#pragma pack(push, 1)
struct MeshBinaryHeader {
    UInt32 magic;              // 'AYMH' = 0x484D5941
    UInt16 version;            // 版本 = 1 (chunked v1)
    UInt16 headerSize;         // = sizeof(MeshBinaryHeader)，loader 用它向前兼容
    FGuid guid;                // 资源唯一标识 (16 bytes)
    UInt32 flags;              // bit 0: hasBounds, bit 1: hasSkinWeights
    UInt8  attributeMask;      // MeshAttribute 位掩码
    UInt8  reserved;           // 对齐 / 未来用
    UInt16 chunkCount;         // chunk 目录项数
    UInt32 chunkTableOffset;   // chunk 表相对文件起始的偏移
    UInt32 vertexCount;        // 顶点数量
    UInt32 indexCount;         // 索引数量
    UInt32 submeshCount;       // Submesh 数量
    UInt32 materialSlotCount;  // Material slot 数量
    Float32 boundsCenter[3];   // 包围盒中心
    Float32 boundsHalfExtent[3]; // 包围盒半尺寸
};                            // 总 76 bytes (UInt32/16, FGuid=16, Float32×6=24, ...，无 padding)
#pragma pack(pop)

// ===== Mesh Chunk 目录项 (12 bytes) =====
#pragma pack(push, 1)
struct MeshChunkDirEntry {
    UInt32 fourCC;     // 'POSN' / 'NORM' / 'UV0 ' / 'TANG' / 'COLR' / 'IDX ' / 'MATL' / 'SUBM' / 'BOUN' / 'SKIN'
    UInt32 offset;     // 相对文件起始的字节偏移
    UInt32 size;       // chunk 字节数
};                      // 总 12 bytes
#pragma pack(pop)

// ===== Mesh Chunk four-cc 常量 (大写 ASCII / little-endian) =====
namespace MeshChunkFourCC {
    constexpr UInt32 POSN = 0x4E534F50; // 'POSN'
    constexpr UInt32 NORM = 0x4D524F4E; // 'NORM'
    constexpr UInt32 UV0  = 0x20305655; // 'UV0 ' (UV + space)
    constexpr UInt32 TANG = 0x474E4154; // 'TANG'
    constexpr UInt32 COLR = 0x524C4F43; // 'COLR'
    constexpr UInt32 IDX  = 0x20584449; // 'IDX ' (IDX + space)
    constexpr UInt32 MATL = 0x4C54414D; // 'MATL'
    constexpr UInt32 SUBM = 0x4D425553; // 'SUBM'
    constexpr UInt32 BOUN = 0x4E554F42; // 'BOUN'
    constexpr UInt32 SKIN = 0x4E494B53; // 'SKIN'
}

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

    // Phase 0 RD-02: construct a skinned mesh in unit tests without going through
    // the binary format. Sets _hasSkinWeights=true, sets the SkinWeight bit in the
    // attribute mask, and resizes the interleaved vertex buffer to match the new
    // stride (pos+norm+uv=32 + skin=24 = 56 bytes/vertex).
    //
    // Existing vertex attributes are preserved; the new skin-weights block is
    // appended to the interleaved stream at offset 32 (i.e. immediately after the
    // UV channel). This mirrors the layout that MeshConverter emits and keeps
    // Phase 1's repack path happy.
    //
    // Production code should NOT call this — the converter sets skin weights via
    // the binary load path. This exists only to let AYRenderer unit tests exercise
    // the upload pipeline deterministically.
    void debugSetSkinWeights(const std::vector<VertexSkinWeight>& weights);

    // ===== Test-only setters for MeshConverter-style input =====
    //
    // These let MeshConverter::saveToBinary build a chunked .aymesh without
    // going through FBX / glTF parse. They are deliberately not in IAYMesh —
    // MeshConverter is the only legitimate caller outside unit tests.
    void _setForTestAttributeMask(UInt8 mask) { _attributeMask = mask; }
    void _setForTestVertexLayout(UInt8 mask, UInt32 vertexCount, UInt32 stride);
    void _setForTestVertexData(const void* data, size_t sizeBytes);
    void _setForTestIndices(const UInt32* indices, UInt32 count);
    void _setForTestSubmeshes(const Submesh* submeshes, UInt32 count);
    void _addForTestMaterialSlot(const std::string& slot);
    void _setForTestSkinWeights(const std::vector<VertexSkinWeight>& weights);
    void _setForTestBounds(const ayt::math::FVector3& center, const ayt::math::FVector3& halfExtent);

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