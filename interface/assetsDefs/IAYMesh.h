#pragma once
#include "IAYResource.h"
#include "AYMath/MathTypes.h"
#include <memory>
#include <vector>

namespace ayt::resource
{

// ===== 顶点属性枚举 =====
enum class MeshAttribute : UInt8 {
    Position = 0,  // 必有: float[3] per vertex
    Normal   = 1,  // 可选: float[3] per vertex
    UV       = 2,  // 可选: float[2] per vertex
    Tangent  = 3,  // 可选: float[4] per vertex (xyz=tangent, w=handedness)
    Color    = 4,  // 可选: float[4] per vertex
    SkinWeight = 5, // 可选: UInt8[4] bone indices + Float32[4] weights
};

// ===== Bounds 结构体 =====
struct Bounds {
    ayt::math::FVector3 center;      // 包围盒中心
    ayt::math::FVector3 halfExtent;  // 半尺寸 (从中心到边缘)

    Bounds() : center(0, 0, 0), halfExtent(0, 0, 0) {}
    Bounds(const ayt::math::FVector3& c, const ayt::math::FVector3& h)
        : center(c), halfExtent(h) {}

    ayt::math::FVector3 getMin() const { return center - halfExtent; }
    ayt::math::FVector3 getMax() const { return center + halfExtent; }

    void setMinMax(const ayt::math::FVector3& min, const ayt::math::FVector3& max) {
        center = (min + max) * 0.5f;
        halfExtent = (max - min) * 0.5f;
    }
};

// ===== 骨骼权重 =====
struct VertexSkinWeight {
    UInt8 boneIndex[4];     // 最多4个骨骼索引
    Float32 boneWeight[4];   // 对应权重 (和为1)
};

// ===== IMesh — 网格资源接口 =====
class IMesh : public IResource {
public:
    virtual ~IMesh() = default;

    // ===== Shared vertex data (交错布局) =====
    virtual UInt32 getVertexCount() const = 0;
    virtual UInt32 getVertexStride() const = 0;      // 单顶点字节大小
    virtual const UInt8* getVertexData() const = 0; // 交错格式

    // ===== Index data =====
    virtual UInt32 getIndexCount() const = 0;
    virtual const UInt32* getIndexData() const = 0;

    // ===== Attributes =====
    virtual UInt8 getAttributeMask() const = 0; // MeshAttribute 位掩码

    struct AttributeInfo {
        UInt8 offset;   // 在 vertex stride 中的字节偏移
        UInt8 count;    // Float32 数量 (3 for position, 2 for uv)
    };
    virtual AttributeInfo getAttributeInfo(MeshAttribute attr) const = 0;

    // ===== Submesh =====
    // F-02 close: wrapped in `#pragma pack(push,1)` so the on-disk SUBM chunk
    // has a deterministic size even if a future field needs 8-byte alignment.
    // F-01 close: `vertexOffset` is appended (last field) so the v1 layout
    // (indexOffset, indexCount, materialIndex) is preserved as a prefix.
#pragma pack(push, 1)
    struct Submesh {
        UInt32 indexOffset;   // 在 indices 数组中的起始位置
        UInt32 indexCount;    // 索引数量
        UInt32 materialIndex; // 索引到 material slots
        UInt32 vertexOffset = 0u; // F-01: 顶点起点(用于 skin-LOD / 按骨骼分区)
    };
#pragma pack(pop)
    static_assert(sizeof(Submesh) == 16, "IMesh::Submesh must be 16 bytes (4 × UInt32 packed)");
    virtual UInt32 getSubmeshCount() const = 0;
    virtual const Submesh* getSubmeshes() const = 0;

    // ===== Material slots =====
    virtual UInt32 getMaterialSlotCount() const = 0;
    virtual const char* getMaterialSlot(UInt32 index) const = 0; // 返回路径字符串

    // ===== Bounds (预计算) =====
    virtual Bounds getBounds() const = 0;
    virtual void getBounds(ayt::math::FVector3& min, ayt::math::FVector3& max) const = 0;
    virtual Bool hasBounds() const = 0;

    // ===== Skin weights (骨骼蒙皮) =====
    virtual Bool hasSkinWeights() const = 0;
    virtual const VertexSkinWeight* getSkinWeights() const = 0;

    // ===== LOD (预留扩展) =====
    struct LODData {
        Float32 distance;  // 切换到这个 LOD 的距离
        Float32 bias;     // 细节偏差
    };
    virtual UInt32 getLODCount() const = 0;
    virtual const LODData* getLODData() const = 0;
    virtual IMesh* getLOD(UInt32 lodIndex) = 0;

    // ===== Extensions (预留扩展槽) =====
    struct Extension {
        UInt32 type;   // 'MORP' | 'CLTH' | 'PHYS' | 'SKEL'
        UInt32 size;   // chunk size
        const UInt8* data;
    };
    virtual UInt32 getExtensionCount() const = 0;
    virtual const Extension* getExtension(UInt32 index) const = 0;
    virtual const Extension* findExtension(UInt32 type) const = 0;

    // ===== Helper methods =====
    inline Bool hasAttribute(MeshAttribute attr) const {
        return (getAttributeMask() & (1u << static_cast<UInt8>(attr))) != 0;
    }

    // ===== 顶点数据访问 (返回 AYMath 类型) =====
    inline const ayt::math::FVector3* getPositions() const {
        if (!hasAttribute(MeshAttribute::Position)) return nullptr;
        return reinterpret_cast<const ayt::math::FVector3*>(
            getVertexData() + getAttributeInfo(MeshAttribute::Position).offset);
    }

    inline const ayt::math::FVector3* getNormals() const {
        if (!hasAttribute(MeshAttribute::Normal)) return nullptr;
        return reinterpret_cast<const ayt::math::FVector3*>(
            getVertexData() + getAttributeInfo(MeshAttribute::Normal).offset);
    }

    inline const ayt::math::FVector2* getUVs() const {
        if (!hasAttribute(MeshAttribute::UV)) return nullptr;
        return reinterpret_cast<const ayt::math::FVector2*>(
            getVertexData() + getAttributeInfo(MeshAttribute::UV).offset);
    }

    inline const ayt::math::FVector4* getTangents() const {
        if (!hasAttribute(MeshAttribute::Tangent)) return nullptr;
        return reinterpret_cast<const ayt::math::FVector4*>(
            getVertexData() + getAttributeInfo(MeshAttribute::Tangent).offset);
    }

    inline const ayt::math::FVector4* getColors() const {
        if (!hasAttribute(MeshAttribute::Color)) return nullptr;
        return reinterpret_cast<const ayt::math::FVector4*>(
            getVertexData() + getAttributeInfo(MeshAttribute::Color).offset);
    }

    // ===== Constants =====
    // v1: Submesh 12 bytes (3 × UInt32)
    // F-01 close: Submesh 16 bytes (4 × UInt32, vertexOffset added; pack(1) for layout safety).
    // 仍是 VERSION = 1 — chunk size 由 dir[i].size 决定, reader 按 chunk 大小读取,
    // 旧 reader 只要 chunk size = N × 12 才会错; 我们的 v1 实现只会写入 N × 16。
    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC = 0x484D5941; // 'AYMH' in little-endian
};

} // namespace ayt::resource