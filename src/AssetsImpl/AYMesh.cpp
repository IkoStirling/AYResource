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

    // Phase 0 RD-02: SkinWeight is reported via hasAttribute() so the
    // RenderAssetBridge picks up the channel, but it is NOT included in the
    // interleaved _vertexData buffer. Skin weights live in a parallel
    // _skinWeights vector (one VertexSkinWeight per vertex) and are written
    // out as a separate trailing block by saveToBinary. This keeps the v1
    // binary format compatible with older readers that ignore the trailing
    // skin-weights block, while letting the renderer see the channel.

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
    using namespace MeshChunkFourCC;

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
    if (header->headerSize != sizeof(MeshBinaryHeader)) {
        ayt::log::error("[Mesh] loadFromBinary failed: headerSize=%u expected=%zu",
                        header->headerSize, sizeof(MeshBinaryHeader));
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取基本属性
    _attributeMask = header->attributeMask;
    _vertexCount   = header->vertexCount;
    _indexCount    = header->indexCount;
    _hasBounds     = (header->flags & 1u) != 0;
    _hasSkinWeights = (header->flags & 2u) != 0
                     || (_attributeMask & (1u << static_cast<UInt8>(MeshAttribute::SkinWeight))) != 0;

    // 读取 bounds (header 中冗余副本)
    if (_hasBounds) {
        _bounds.center.x = header->boundsCenter[0];
        _bounds.center.y = header->boundsCenter[1];
        _bounds.center.z = header->boundsCenter[2];
        _bounds.halfExtent.x = header->boundsHalfExtent[0];
        _bounds.halfExtent.y = header->boundsHalfExtent[1];
        _bounds.halfExtent.z = header->boundsHalfExtent[2];
    }

    // 重新计算 vertex stride (chunked 文件下 _vertexData 会被重新交织)
    _computeVertexStride();
    const UInt32 V = _vertexCount;
    const UInt32 stride = _vertexStride;
    if (V > 0 && stride > 0) {
        _vertexData.assign(static_cast<size_t>(V) * stride, 0);
    }

    // 读取 chunk dir
    const UInt32 chunkCount = header->chunkCount;
    const UInt32 dirOffset  = header->chunkTableOffset;
    if (chunkCount == 0) {
        // 没有 chunk 也要有 IDX / MATL / SUBM —— 但保留宽松语义，仅当它们确实缺失时不报错
    } else {
        if (dirOffset + chunkCount * sizeof(MeshChunkDirEntry) > size) {
            ayt::log::error("[Mesh] loadFromBinary: chunk dir out of range (off=%u count=%u size=%zu)",
                            dirOffset, chunkCount, size);
            return false;
        }
        const MeshChunkDirEntry* dir =
            reinterpret_cast<const MeshChunkDirEntry*>(ptr + dirOffset);

        // helper: 计算 attribute 在 _vertexData 内的 offset/count
        auto getAttrOffsetCount = [&](MeshAttribute a, UInt32& attrOffset, UInt32& attrCount) {
            attrOffset = 0; attrCount = 0;
            const UInt8 mask = _attributeMask;
            UInt32 cur = 0;
            if (mask & (1u << static_cast<UInt8>(MeshAttribute::Position))) {
                if (a == MeshAttribute::Position) { attrOffset = cur; attrCount = 3; } cur += 12;
            }
            if (mask & (1u << static_cast<UInt8>(MeshAttribute::Normal))) {
                if (a == MeshAttribute::Normal)   { attrOffset = cur; attrCount = 3; } cur += 12;
            }
            if (mask & (1u << static_cast<UInt8>(MeshAttribute::UV))) {
                if (a == MeshAttribute::UV)       { attrOffset = cur; attrCount = 2; } cur += 8;
            }
            if (mask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
                if (a == MeshAttribute::Tangent)  { attrOffset = cur; attrCount = 4; } cur += 16;
            }
            if (mask & (1u << static_cast<UInt8>(MeshAttribute::Color))) {
                if (a == MeshAttribute::Color)    { attrOffset = cur; attrCount = 4; } cur += 16;
            }
        };

        for (UInt32 i = 0; i < chunkCount; ++i) {
            const UInt32 fourCC = dir[i].fourCC;
            const UInt32 off    = dir[i].offset;
            const UInt32 sz     = dir[i].size;

            if (off + sz > size) {
                ayt::log::error("[Mesh] loadFromBinary: chunk '%c%c%c%c' out of range (off=%u sz=%u file=%zu)",
                                (char)(fourCC & 0xFF), (char)((fourCC >> 8) & 0xFF),
                                (char)((fourCC >> 16) & 0xFF), (char)((fourCC >> 24) & 0xFF),
                                off, sz, size);
                return false;
            }
            const UInt8* cdata = ptr + off;

            switch (fourCC) {
            case POSN:
            case NORM:
            case UV0:
            case TANG:
            case COLR: {
                MeshAttribute attr = MeshAttribute::Position;
                if      (fourCC == POSN) attr = MeshAttribute::Position;
                else if (fourCC == NORM) attr = MeshAttribute::Normal;
                else if (fourCC == UV0)  attr = MeshAttribute::UV;
                else if (fourCC == TANG) attr = MeshAttribute::Tangent;
                else if (fourCC == COLR) attr = MeshAttribute::Color;
                else break;

                if (!(_attributeMask & (1u << static_cast<UInt8>(attr)))) {
                    ayt::log::error("[Mesh] loadFromBinary: chunk '%c%c%c%c' present but attributeMask bit %u unset",
                                    (char)(fourCC & 0xFF), (char)((fourCC >> 8) & 0xFF),
                                    (char)((fourCC >> 16) & 0xFF), (char)((fourCC >> 24) & 0xFF),
                                    static_cast<UInt32>(attr));
                    return false;
                }

                UInt32 attrOffset = 0, attrCount = 0;
                getAttrOffsetCount(attr, attrOffset, attrCount);
                const size_t expected = static_cast<size_t>(V) * attrCount * sizeof(float);
                if (sz != expected) {
                    ayt::log::error("[Mesh] loadFromBinary: chunk '%c%c%c%c' size=%u expected=%zu",
                                    (char)(fourCC & 0xFF), (char)((fourCC >> 8) & 0xFF),
                                    (char)((fourCC >> 16) & 0xFF), (char)((fourCC >> 24) & 0xFF),
                                    sz, expected);
                    return false;
                }
                // 散射回 _vertexData
                for (UInt32 v = 0; v < V; ++v) {
                    const float* src = reinterpret_cast<const float*>(
                        cdata + static_cast<size_t>(v) * attrCount * sizeof(float));
                    float* dst = reinterpret_cast<float*>(
                        _vertexData.data() + static_cast<size_t>(v) * stride + attrOffset);
                    for (UInt32 c = 0; c < attrCount; ++c) {
                        dst[c] = src[c];
                    }
                }
                break;
            }
            case IDX: {
                const size_t expected = static_cast<size_t>(_indexCount) * sizeof(UInt32);
                if (sz != expected) {
                    ayt::log::error("[Mesh] loadFromBinary: IDX size=%u expected=%zu", sz, expected);
                    return false;
                }
                _indices.resize(_indexCount);
                std::memcpy(_indices.data(), cdata, sz);
                break;
            }
            case MATL: {
                _materialSlots.clear();
                size_t pos = 0;
                for (UInt32 s = 0; s < header->materialSlotCount; ++s) {
                    if (pos + sizeof(UInt32) > sz) return false;
                    UInt32 len = *reinterpret_cast<const UInt32*>(cdata + pos);
                    pos += sizeof(UInt32);
                    if (pos + len > sz) return false;
                    _materialSlots.emplace_back(reinterpret_cast<const char*>(cdata + pos), len);
                    pos += len;
                }
                break;
            }
            case SUBM: {
                const size_t expected = static_cast<size_t>(header->submeshCount) * sizeof(Submesh);
                if (sz != expected) {
                    ayt::log::error("[Mesh] loadFromBinary: SUBM size=%u expected=%zu", sz, expected);
                    return false;
                }
                _submeshes.resize(header->submeshCount);
                std::memcpy(_submeshes.data(), cdata, sz);
                break;
            }
            case BOUN: {
                if (sz != 6u * sizeof(float)) {
                    ayt::log::error("[Mesh] loadFromBinary: BOUN size=%u expected=24", sz);
                    return false;
                }
                // _hasBounds 之前可能已由 flags 决定；这里以 chunk 为权威源覆盖
                _bounds.center.x = (*reinterpret_cast<const float*>(cdata + 0));
                _bounds.center.y = (*reinterpret_cast<const float*>(cdata + 4));
                _bounds.center.z = (*reinterpret_cast<const float*>(cdata + 8));
                _bounds.halfExtent.x = (*reinterpret_cast<const float*>(cdata + 12));
                _bounds.halfExtent.y = (*reinterpret_cast<const float*>(cdata + 16));
                _bounds.halfExtent.z = (*reinterpret_cast<const float*>(cdata + 20));
                _hasBounds = true;
                break;
            }
            case SKIN: {
                const size_t expected = static_cast<size_t>(_vertexCount) * sizeof(VertexSkinWeight);
                if (sz != expected) {
                    ayt::log::error("[Mesh] loadFromBinary: SKIN size=%u expected=%zu", sz, expected);
                    return false;
                }
                _skinWeights.resize(_vertexCount);
                std::memcpy(_skinWeights.data(), cdata, sz);
                _hasSkinWeights = true;
                break;
            }
            default:
                // 未知 four-cc：忽略，记录但不失败（向前兼容）
                ayt::log::debug("[Mesh] loadFromBinary: ignoring unknown chunk '%c%c%c%c' (size=%u)",
                                (char)(fourCC & 0xFF), (char)((fourCC >> 8) & 0xFF),
                                (char)((fourCC >> 16) & 0xFF), (char)((fourCC >> 24) & 0xFF),
                                sz);
                break;
            }
        }
    }

    // 运行时计算 bounds（如果没有预计算）
    if (!_hasBounds) {
        _computeBounds();
    }

    _loaded = true;
    return true;
}

// ===== Test-only setters (MeshConverter 专用入口) =====
//
// 这些 setter 让 MeshConverter::saveToBinary 把 MeshData 灌进一个临时 Mesh 实例，
// 进而复用 Mesh::saveToBinary 的 chunked v1 输出。这些方法必须保持公开，但
// 仅在 Converter + 单元测试中调用。

void Mesh::_setForTestVertexLayout(UInt8 mask, UInt32 vertexCount, UInt32 stride)
{
    _attributeMask = mask;
    _vertexCount   = vertexCount;
    _vertexStride  = stride;
    _vertexData.assign(static_cast<size_t>(vertexCount) * stride, 0);
    // 重新刷新 _attrInfo（调试用）
    _computeVertexStride();
}

void Mesh::_setForTestVertexData(const void* data, size_t sizeBytes)
{
    if (data == nullptr || sizeBytes == 0) {
        _vertexData.clear();
        return;
    }
    _vertexData.resize(sizeBytes);
    std::memcpy(_vertexData.data(), data, sizeBytes);
}

void Mesh::_setForTestIndices(const UInt32* indices, UInt32 count)
{
    _indexCount = count;
    _indices.resize(count);
    if (count > 0 && indices != nullptr) {
        std::memcpy(_indices.data(), indices, count * sizeof(UInt32));
    }
}

void Mesh::_setForTestSubmeshes(const Submesh* submeshes, UInt32 count)
{
    _submeshes.assign(submeshes, submeshes + count);
}

void Mesh::_addForTestMaterialSlot(const std::string& slot)
{
    _materialSlots.push_back(slot);
}

void Mesh::_setForTestSkinWeights(const std::vector<VertexSkinWeight>& weights)
{
    _skinWeights = weights;
    _hasSkinWeights = !weights.empty();
    if (_hasSkinWeights) {
        _attributeMask |= (1u << static_cast<UInt8>(MeshAttribute::SkinWeight));
    }
}

void Mesh::_setForTestBounds(const ayt::math::FVector3& center, const ayt::math::FVector3& halfExtent)
{
    _bounds.setMinMax(center - halfExtent, center + halfExtent);
    _hasBounds = true;
}

// ===== saveToBinary (chunked v1 layout) =====
//
// 文件 layout:
//   [MeshBinaryHeader] [ChunkDirectory][N × 12] [chunk 0] [chunk 1] ... [chunk N-1]
//
// chunks 以 four-cc 标识，可任意顺序；mesh 通过 chunkTableOffset + chunkCount
// 找到目录，再按目录寻址各 chunk。多通道顶点的每个通道是一个独立 chunk (POSN/NORM/...)。
// Skin weight 走独立 'SKIN' chunk，不再与 vertex stream 串在同一块 interleaved bytes 里。
bool Mesh::saveToBinary(std::vector<UInt8>& outData) const {
    using namespace MeshChunkFourCC;

    // ---------- 1) 决定要写入哪些 chunk ----------
    struct ChunkSpec {
        UInt32 fourCC;
        std::vector<UInt8> data;
    };

    std::vector<ChunkSpec> chunks;

    // 注意：调用 saveToBinary 之前必须保证 _vertexStride 与 _vertexData layout 同步——
    // 例如 loadFromBinary / createCube / createSphere / _setForTest* 已经调用过 _computeVertexStride。
    // saveToBinary 是 const 方法，无法再次刷新 stride，由调用方负责。

    const UInt32 V = _vertexCount;
    const UInt32 stride = _vertexStride;
    const UInt8* base = _vertexData.empty() ? nullptr : _vertexData.data();

    // 任意 attribute 的 float stream 切出一个独立 chunk
    auto pushFloatAttrChunk = [&](UInt32 fourCC, MeshAttribute a, UInt32 attrCount) {
        if (!(_attributeMask & (1u << static_cast<UInt8>(a)))) {
            return;
        }
        UInt32 attrOffset = 0;
        UInt32 cur = 0;
        const UInt8 mask = _attributeMask;
        if (mask & (1u << static_cast<UInt8>(MeshAttribute::Position))) {
            if (a == MeshAttribute::Position) attrOffset = cur; cur += 12;
        }
        if (mask & (1u << static_cast<UInt8>(MeshAttribute::Normal))) {
            if (a == MeshAttribute::Normal)   attrOffset = cur; cur += 12;
        }
        if (mask & (1u << static_cast<UInt8>(MeshAttribute::UV))) {
            if (a == MeshAttribute::UV)       attrOffset = cur; cur += 8;
        }
        if (mask & (1u << static_cast<UInt8>(MeshAttribute::Tangent))) {
            if (a == MeshAttribute::Tangent)  attrOffset = cur; cur += 16;
        }
        if (mask & (1u << static_cast<UInt8>(MeshAttribute::Color))) {
            if (a == MeshAttribute::Color)    attrOffset = cur; cur += 16;
        }

        ChunkSpec ch{fourCC, std::vector<UInt8>(static_cast<size_t>(attrCount) * V * sizeof(Float32))};
        for (UInt32 v = 0; v < V; ++v) {
            const UInt8* src = base + static_cast<size_t>(v) * stride + attrOffset;
            std::memcpy(ch.data.data() + static_cast<size_t>(v) * attrCount * sizeof(Float32),
                        src, attrCount * sizeof(Float32));
        }
        chunks.push_back(std::move(ch));
    };

    pushFloatAttrChunk(POSN, MeshAttribute::Position, 3);
    pushFloatAttrChunk(NORM, MeshAttribute::Normal,   3);
    pushFloatAttrChunk(UV0,  MeshAttribute::UV,       2);
    pushFloatAttrChunk(TANG, MeshAttribute::Tangent,  4);
    pushFloatAttrChunk(COLR, MeshAttribute::Color,    4);

    // IDX
    {
        const UInt32 idxSize = _indexCount * sizeof(UInt32);
        ChunkSpec s{IDX, std::vector<UInt8>(idxSize)};
        if (idxSize > 0) {
            std::memcpy(s.data.data(), _indices.data(), idxSize);
        }
        chunks.push_back(std::move(s));
    }

    // MATL: [slotCount]{ UInt32 nameLen; char name[]; }
    {
        std::vector<UInt8> buf;
        for (const auto& slot : _materialSlots) {
            UInt32 len = static_cast<UInt32>(slot.size());
            const size_t pos = buf.size();
            buf.resize(pos + sizeof(UInt32) + len);
            std::memcpy(buf.data() + pos, &len, sizeof(UInt32));
            if (len > 0) {
                std::memcpy(buf.data() + pos + sizeof(UInt32), slot.data(), len);
            }
        }
        chunks.push_back({MATL, std::move(buf)});
    }

    // SUBM
    {
        const size_t sz = _submeshes.size() * sizeof(Submesh);
        ChunkSpec s{SUBM, std::vector<UInt8>(sz)};
        if (sz > 0) {
            std::memcpy(s.data.data(), _submeshes.data(), sz);
        }
        chunks.push_back(std::move(s));
    }

    // BOUN (optional)
    if (_hasBounds) {
        float buf[6] = {
            _bounds.center.x, _bounds.center.y, _bounds.center.z,
            _bounds.halfExtent.x, _bounds.halfExtent.y, _bounds.halfExtent.z,
        };
        ChunkSpec ch{BOUN, std::vector<UInt8>(sizeof(buf))};
        std::memcpy(ch.data.data(), buf, sizeof(buf));
        chunks.push_back(std::move(ch));
    }

    // SKIN (optional)
    if (_hasSkinWeights && !_skinWeights.empty()) {
        const UInt32 sz = _vertexCount * sizeof(VertexSkinWeight);
        ChunkSpec ch{SKIN, std::vector<UInt8>(sz)};
        std::memcpy(ch.data.data(), _skinWeights.data(), sz);
        chunks.push_back(std::move(ch));
    }

    // ---------- 2) 计算 chunk 偏移 (4 字节对齐) ----------
    const UInt32 headerSize    = static_cast<UInt32>(sizeof(MeshBinaryHeader));
    const UInt32 chunkTableSz  = static_cast<UInt32>(chunks.size() * sizeof(MeshChunkDirEntry));
    const UInt32 chunkTableOff = headerSize;

    UInt32 cursor = chunkTableOff + chunkTableSz;
    std::vector<MeshChunkDirEntry> dir(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        // 4 字节对齐
        const UInt32 pad = (4u - (cursor & 3u)) & 3u;
        cursor += pad;
        dir[i].fourCC = chunks[i].fourCC;
        dir[i].offset = cursor;
        dir[i].size   = static_cast<UInt32>(chunks[i].data.size());
        cursor += dir[i].size;
    }
    const UInt32 totalSize = cursor;

    // ---------- 3) 写 header + dir + chunks ----------
    outData.assign(totalSize, 0);
    UInt8* out = outData.data();

    MeshBinaryHeader header{};
    header.magic              = IMesh::MAGIC;
    header.version            = IMesh::VERSION;
    header.headerSize         = headerSize;
    header.guid               = _guid;
    header.flags              = (_hasBounds ? 1u : 0u)
                              | (_hasSkinWeights ? 2u : 0u);
    header.attributeMask      = _attributeMask;
    header.chunkCount         = static_cast<UInt16>(chunks.size());
    header.chunkTableOffset   = chunkTableOff;
    header.vertexCount        = _vertexCount;
    header.indexCount         = _indexCount;
    header.submeshCount       = static_cast<UInt32>(_submeshes.size());
    header.materialSlotCount  = static_cast<UInt32>(_materialSlots.size());
    if (_hasBounds) {
        header.boundsCenter[0]    = _bounds.center.x;
        header.boundsCenter[1]    = _bounds.center.y;
        header.boundsCenter[2]    = _bounds.center.z;
        header.boundsHalfExtent[0] = _bounds.halfExtent.x;
        header.boundsHalfExtent[1] = _bounds.halfExtent.y;
        header.boundsHalfExtent[2] = _bounds.halfExtent.z;
    }
    std::memcpy(out, &header, sizeof(header));

    if (!dir.empty()) {
        std::memcpy(out + chunkTableOff, dir.data(), chunkTableSz);
    }

    // chunks
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (!chunks[i].data.empty()) {
            std::memcpy(out + dir[i].offset, chunks[i].data.data(), chunks[i].data.size());
        }
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

// Phase 0 RD-02: test helper that adds skin weights to an existing Mesh.
//
// Assumes the caller has already populated pos/norm/uv via createCube/createSphere
// (or via the binary load path). The SkinWeight channel is reported via the
// attribute mask so RenderAssetBridge picks it up, but the actual weight data
// lives in the parallel _skinWeights vector (NOT in the interleaved vertex
// buffer). This matches saveToBinary/loadFromBinary v1 layout and the
// repack path's contract.
//
// Effect:
//   attributeMask |= (1 << SkinWeight)
//   _attrInfo[5]   = { offset = 0, count = 4 }   // sentinel; not used by repack
//   _vertexStride  unchanged                    // skin not interleaved
//   _skinWeights   = weights                     // parallel vector
//   _hasSkinWeights = true
//
// Production code should NOT call this — the converter sets skin weights via
// the binary load path. This exists only to let AYRenderer unit tests exercise
// the upload pipeline deterministically.
void Mesh::debugSetSkinWeights(const std::vector<VertexSkinWeight>& weights)
{
    if (weights.empty()) {
        return;
    }
    if (_vertexCount == 0) {
        return;
    }
    if (weights.size() != _vertexCount) {
        ayt::log::error("[Mesh] debugSetSkinWeights: weights.size()=%zu != vertexCount=%u",
                        weights.size(), _vertexCount);
        return;
    }

    constexpr UInt8 kSkinWeightBit = static_cast<UInt8>(MeshAttribute::SkinWeight);

    // 1. flip the mask bit
    _attributeMask |= (1u << kSkinWeightBit);

    // 2. populate layout info (offset is informational; repack reads from
    //    getSkinWeights() directly, not via attribute offsets).
    _attrInfo[5] = AttributeInfo{};
    _attrInfo[5].offset = 0;
    _attrInfo[5].count  = 4;

    // 3. populate skin weights
    _skinWeights = weights;
    _hasSkinWeights = true;
}

}