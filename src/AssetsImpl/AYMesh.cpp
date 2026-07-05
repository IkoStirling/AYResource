#include "AYMesh.h"
#include "AYIO.h"
#include "AYMathTypes.h"
#include "AYMathUtils.h"
#include <AYLog.h>

namespace ayt::resource
{
// ===== Mesh =====

Mesh::Mesh() = default;

void Mesh::_clear() {
    _vertexData.clear();
    _indices.clear();
    _submeshes.clear();
    _materialSlots.clear();
    _extensions.clear();
    _lodData.clear();
    _lods.clear();
    _hasBounds = false;
    _vertexCount = 0;
    _vertexStride = 0;
    _attributeMask = 0;
}

bool Mesh::unload() {
    _clear();
    _loaded = false;
    return true;
}

size_t Mesh::sizeInBytes() const {
    return sizeof(Mesh) + _vertexData.size() + _indices.size() * sizeof(UInt32);
}

void Mesh::_computeBounds() {
    if (!hasAttribute(MeshAttribute::Position)) {
        _hasBounds = false;
        return;
    }

    const UInt8* vertexData = getVertexData();
    auto posInfo = getAttributeInfo(MeshAttribute::Position);
    if (!vertexData) {
        _hasBounds = false;
        return;
    }

    const UInt8* firstPosPtr = vertexData + posInfo.offset;
    ayt::math::FVector3 vmin = *reinterpret_cast<const ayt::math::FVector3*>(firstPosPtr);
    ayt::math::FVector3 vmax = vmin;

    for (UInt32 i = 1; i < _vertexCount; i++) {
        const UInt8* ptr = vertexData + i * _vertexStride + posInfo.offset;
        const ayt::math::FVector3& pos = *reinterpret_cast<const ayt::math::FVector3*>(ptr);
        vmin.x = ayt::math::min(vmin.x, pos.x);
        vmin.y = ayt::math::min(vmin.y, pos.y);
        vmin.z = ayt::math::min(vmin.z, pos.z);
        vmax.x = ayt::math::max(vmax.x, pos.x);
        vmax.y = ayt::math::max(vmax.y, pos.y);
        vmax.z = ayt::math::max(vmax.z, pos.z);
    }

    _bounds.setMinMax(vmin, vmax);
    _hasBounds = true;
}

void Mesh::_computeVertexStride() {
    _vertexStride = 0;
    UInt8 mask = _attributeMask;

    AttributeInfo info;
    info.offset = 0;

    if (mask & (1u << static_cast<UInt8>(MeshAttribute::Position))) {
        info.count = 3;
        _attrInfo[0] = info;
        info.offset += 3 * sizeof(Float32);
    }

    if (mask & (1u << static_cast<UInt8>(MeshAttribute::Normal))) {
        info.count = 3;
        _attrInfo[1] = info;
        info.offset += 3 * sizeof(Float32);
    }

    if (mask & (1u << static_cast<UInt8>(MeshAttribute::UV))) {
        info.count = 2;
        _attrInfo[2] = info;
        info.offset += 2 * sizeof(Float32);
    }

    if (mask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
        info.count = 4;
        _attrInfo[3] = info;
        info.offset += 4 * sizeof(Float32);
    }

    if (mask & (1u << static_cast<UInt8>(MeshAttribute::Color))) {
        info.count = 4;
        _attrInfo[4] = info;
        info.offset += 4 * sizeof(Float32);
    }

    _vertexStride = info.offset;
}

bool Mesh::load(const std::string& path) {
    _path = path;

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(MeshBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Mesh::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(MeshBinaryHeader)) {
        ayt::log::error("[Mesh] loadFromBinary failed: invalid data or size=%zu", size);
        return false;
    }

    _clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const MeshBinaryHeader* header = reinterpret_cast<const MeshBinaryHeader*>(ptr);

    // 验证 magic 和 version
    if (header->magic != IMesh::MAGIC || header->version != IMesh::VERSION) {
        ayt::log::error("[Mesh] loadFromBinary failed: magic=0x%08X version=%d expected magic=0x%08X version=%d",
                        header->magic, header->version, IMesh::MAGIC, IMesh::VERSION);
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取基本属性
    _attributeMask = header->attributeMask;
    _vertexCount = header->vertexCount;
    _indexCount = header->indexCount;
    _hasBounds = header->hasBounds != 0;
    _hasSkinWeights = header->hasSkinWeights != 0;

    // 读取 bounds
    if (_hasBounds) {
        _bounds.center.x = header->boundsCenter[0];
        _bounds.center.y = header->boundsCenter[1];
        _bounds.center.z = header->boundsCenter[2];
        _bounds.halfExtent.x = header->boundsHalfExtent[0];
        _bounds.halfExtent.y = header->boundsHalfExtent[1];
        _bounds.halfExtent.z = header->boundsHalfExtent[2];
    }

    // 重新计算 vertex stride
    _computeVertexStride();

    // 读取顺序必须与 saveToBinary 一致：vertexData → indices → materialSlots → submeshes → skinWeights

    size_t offset = sizeof(MeshBinaryHeader);

    // 读取 vertexData
    size_t vertexDataSize = _vertexCount * _vertexStride;
    if (offset + vertexDataSize > size) {
        return false;
    }
    _vertexData.resize(vertexDataSize);
    std::memcpy(_vertexData.data(), ptr + offset, vertexDataSize);
    offset += vertexDataSize;

    // 读取 indices
    size_t indicesSize = _indexCount * sizeof(UInt32);
    if (offset + indicesSize > size) {
        return false;
    }
    _indices.resize(_indexCount);
    std::memcpy(_indices.data(), ptr + offset, indicesSize);
    offset += indicesSize;

    // 读取 materialSlots
    _materialSlots.resize(header->materialSlotCount);
    for (UInt32 i = 0; i < header->materialSlotCount; i++) {
        if (offset + sizeof(UInt32) > size) {
            return false;
        }
        UInt32 strLen = *reinterpret_cast<const UInt32*>(ptr + offset);
        offset += sizeof(UInt32);
        if (offset + strLen > size) {
            return false;
        }
        _materialSlots[i].assign(reinterpret_cast<const char*>(ptr + offset), strLen);
        offset += strLen;
    }

    // 读取 submeshes
    _submeshes.resize(header->submeshCount);
    for (UInt32 i = 0; i < header->submeshCount; i++) {
        if (offset + sizeof(Submesh) > size) {
            return false;
        }
        std::memcpy(&_submeshes[i], ptr + offset, sizeof(Submesh));
        offset += sizeof(Submesh);
    }

    // 读取 skin weights
    if (_hasSkinWeights) {
        size_t skinWeightsSize = _vertexCount * sizeof(VertexSkinWeight);
        if (offset + skinWeightsSize > size) {
            return false;
        }
        _skinWeights.resize(_vertexCount);
        std::memcpy(_skinWeights.data(), ptr + offset, skinWeightsSize);
        offset += skinWeightsSize;
    }

    // 运行时计算 bounds（如果没有预计算）
    if (!_hasBounds) {
        _computeBounds();
    }

    _loaded = true;
    return true;
}

bool Mesh::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t materialSlotsSize = 0;
    for (const auto& slot : _materialSlots) {
        materialSlotsSize += sizeof(UInt32) + slot.size();
    }
    size_t vertexDataSize = _vertexCount * _vertexStride;
    size_t indicesSize = _indexCount * sizeof(UInt32);
    size_t submeshesSize = _submeshes.size() * sizeof(Submesh);
    size_t skinWeightsSize = _hasSkinWeights ? _vertexCount * sizeof(VertexSkinWeight) : 0;
    size_t totalSize = sizeof(MeshBinaryHeader) + materialSlotsSize + vertexDataSize + indicesSize + submeshesSize + skinWeightsSize;

    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Custom Header
    MeshBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = IMesh::MAGIC;
    header.version = IMesh::VERSION;
    header.guid = _guid;
    header.attributeMask = _attributeMask;
    header.flags = _hasSkinWeights ? 1 : 0;
    header.vertexCount = _vertexCount;
    header.indexCount = _indexCount;
    header.submeshCount = static_cast<UInt32>(_submeshes.size());
    header.materialSlotCount = static_cast<UInt32>(_materialSlots.size());
    header.hasBounds = _hasBounds ? 1 : 0;
    header.hasSkinWeights = _hasSkinWeights ? 1 : 0;

    if (_hasBounds) {
        header.boundsCenter[0] = _bounds.center.x;
        header.boundsCenter[1] = _bounds.center.y;
        header.boundsCenter[2] = _bounds.center.z;
        header.boundsHalfExtent[0] = _bounds.halfExtent.x;
        header.boundsHalfExtent[1] = _bounds.halfExtent.y;
        header.boundsHalfExtent[2] = _bounds.halfExtent.z;
    }

    std::memcpy(ptr, &header, sizeof(header));

    // 写入顺序：vertexData → indices → materialSlots → submeshes → skinWeights
    // （与 MeshConverter::saveToBinary 一致）
    size_t offset = sizeof(MeshBinaryHeader);

    // 写入 vertexData
    if (vertexDataSize > 0) {
        std::memcpy(ptr + offset, _vertexData.data(), vertexDataSize);
        offset += vertexDataSize;
    }

    // 写入 indices
    if (indicesSize > 0) {
        std::memcpy(ptr + offset, _indices.data(), indicesSize);
        offset += indicesSize;
    }

    // 写入 materialSlots
    for (const auto& slot : _materialSlots) {
        UInt32 strLen = static_cast<UInt32>(slot.size());
        std::memcpy(ptr + offset, &strLen, sizeof(UInt32));
        offset += sizeof(UInt32);
        if (strLen > 0) {
            std::memcpy(ptr + offset, slot.data(), strLen);
            offset += strLen;
        }
    }

    // 写入 submeshes
    for (const auto& submesh : _submeshes) {
        std::memcpy(ptr + offset, &submesh, sizeof(Submesh));
        offset += sizeof(Submesh);
    }

    // 写入 skin weights
    if (_hasSkinWeights && !_skinWeights.empty()) {
        std::memcpy(ptr + offset, _skinWeights.data(), skinWeightsSize);
        offset += skinWeightsSize;
    }

    return true;
}

const IMesh::Extension* Mesh::findExtension(UInt32 type) const {
    for (const auto& ext : _extensions) {
        if (ext.type == type) {
            return &ext;
        }
    }
    return nullptr;
}

// ===== 创建测试数据 =====

void Mesh::createCube(Float32 size) {
    _clear();

    _attributeMask = (1u << static_cast<UInt8>(MeshAttribute::Position)) |
        (1u << static_cast<UInt8>(MeshAttribute::Normal)) |
        (1u << static_cast<UInt8>(MeshAttribute::UV));
    _computeVertexStride();

    // 8 顶点, 12 三角 (每面2个), 36 索引
    _vertexCount = 24;  // 每面4个独立顶点 (法线和UV不同)
    _indexCount = 36;
    _submeshes.resize(1);
    _submeshes[0] = { 0, 36, 0 };
    _materialSlots.resize(1);
    _materialSlots[0] = "default.aymat";

    _vertexData.resize(_vertexCount * _vertexStride);
    _indices.resize(_indexCount);

    Float32 h = size * 0.5f;

    // 面: +X, -X, +Y, -Y, +Z, -Z
    // 每面4顶点: 右下, 右下+1, 左上, 左上+1 (带法线向外, UV正确)
    struct Vertex {
        Float32 x, y, z;   // position
        Float32 nx, ny, nz; // normal
        Float32 u, v;       // uv
    };

    Vertex vertices[24] = {
        // +X face
        { h, -h,  h,  1, 0, 0,  0, 1},
        { h, -h, -h,  1, 0, 0,  1, 1},
        { h,  h, -h,  1, 0, 0,  1, 0},
        { h,  h,  h,  1, 0, 0,  0, 0},
        // -X face
        {-h, -h, -h, -1, 0, 0,  0, 1},
        {-h, -h,  h, -1, 0, 0,  1, 1},
        {-h,  h,  h, -1, 0, 0,  1, 0},
        {-h,  h, -h, -1, 0, 0,  0, 0},
        // +Y face
        {-h,  h, -h,  0, 1, 0,  0, 1},
        {-h,  h,  h,  0, 1, 0,  1, 1},
        { h,  h,  h,  0, 1, 0,  1, 0},
        { h,  h, -h,  0, 1, 0,  0, 0},
        // -Y face
        {-h, -h,  h,  0,-1, 0,  0, 1},
        {-h, -h, -h,  0,-1, 0,  1, 1},
        { h, -h, -h,  0,-1, 0,  1, 0},
        { h, -h,  h,  0,-1, 0,  0, 0},
        // +Z face
        {-h, -h,  h,  0, 0, 1,  0, 1},
        { h, -h,  h,  0, 0, 1,  1, 1},
        { h,  h,  h,  0, 0, 1,  1, 0},
        {-h,  h,  h,  0, 0, 1,  0, 0},
        // -Z face
        { h, -h, -h,  0, 0,-1,  0, 1},
        {-h, -h, -h,  0, 0,-1,  1, 1},
        {-h,  h, -h,  0, 0,-1,  1, 0},
        { h,  h, -h,  0, 0,-1,  0, 0},
    };

    // CCW front faces for bgfx BGFX_STATE_CULL_CW (matches renderer unit cube winding).
    UInt32 indices[36] = {
            0,  2,  1,   0,  3,  2,  // +X
            4,  6,  5,   4,  7,  6,  // -X
            8, 10,  9,   8, 11, 10,  // +Y
        12, 14, 13,  12, 15, 14,  // -Y
        16, 18, 17,  16, 19, 18,  // +Z
        20, 22, 21,  20, 23, 22   // -Z
    };

    std::memcpy(_vertexData.data(), vertices, sizeof(vertices));
    std::memcpy(_indices.data(), indices, sizeof(indices));

    _computeBounds();
    _loaded = true;
}

void Mesh::createSphere(Float32 radius, UInt32 segments) {
    _clear();

    _attributeMask = (1u << static_cast<UInt8>(MeshAttribute::Position)) |
        (1u << static_cast<UInt8>(MeshAttribute::Normal)) |
        (1u << static_cast<UInt8>(MeshAttribute::UV));
    _computeVertexStride();

    // 球体: (segments+1) x (segments+1) 顶点, 2 x segments x segments 三角形
    _vertexCount = (segments + 1) * (segments + 1);
    _indexCount = segments * segments * 6;
    _submeshes.resize(1);
    _submeshes[0] = { 0, _indexCount, 0 };
    _materialSlots.resize(1);
    _materialSlots[0] = "default.aymat";

    _vertexData.resize(_vertexCount * _vertexStride);
    _indices.resize(_indexCount);

    // 生成球体顶点
    for (UInt32 lat = 0; lat <= segments; lat++) {
        Float32 theta = static_cast<Float32>(lat) * MATH_PI / segments;
        Float32 sinTheta = ayt::math::sin(theta);
        Float32 cosTheta = ayt::math::cos(theta);

        for (UInt32 lon = 0; lon <= segments; lon++) {
            Float32 phi = static_cast<Float32>(lon) * 2.0f * MATH_PI / segments;
            Float32 sinPhi = ayt::math::sin(phi);
            Float32 cosPhi = ayt::math::cos(phi);

            Float32 x = cosPhi * sinTheta;
            Float32 y = cosTheta;
            Float32 z = sinPhi * sinTheta;

            UInt32 idx = lat * (segments + 1) + lon;
            UInt32 offset = idx * _vertexStride;

            // Position
            *reinterpret_cast<Float32*>(_vertexData.data() + offset) = radius * x;
            *reinterpret_cast<Float32*>(_vertexData.data() + offset + 4) = radius * y;
            *reinterpret_cast<Float32*>(_vertexData.data() + offset + 8) = radius * z;

            // Normal (normalized position for sphere)
            *reinterpret_cast<Float32*>(_vertexData.data() + offset + 12) = x;
            *reinterpret_cast<Float32*>(_vertexData.data() + offset + 16) = y;
            *reinterpret_cast<Float32*>(_vertexData.data() + offset + 20) = z;

            // UV
            *reinterpret_cast<Float32*>(_vertexData.data() + offset + 24) = static_cast<Float32>(lon) / segments;
            *reinterpret_cast<Float32*>(_vertexData.data() + offset + 28) = static_cast<Float32>(lat) / segments;
        }
    }

    // 生成索引
    UInt32 indexIdx = 0;
    for (UInt32 lat = 0; lat < segments; lat++) {
        for (UInt32 lon = 0; lon < segments; lon++) {
            UInt32 current = lat * (segments + 1) + lon;
            UInt32 next = current + segments + 1;

            _indices[indexIdx++] = current;
            _indices[indexIdx++] = next;
            _indices[indexIdx++] = current + 1;

            _indices[indexIdx++] = current + 1;
            _indices[indexIdx++] = next;
            _indices[indexIdx++] = next + 1;
        }
    }

    _computeBounds();
    _loaded = true;
}

}