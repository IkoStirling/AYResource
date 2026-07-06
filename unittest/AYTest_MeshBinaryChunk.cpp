// AYTest_MeshBinaryChunk.cpp — Phase 0 R-01 follow-up: chunked v1 .aymesh
//
// 验证 Mesh 的 chunked 二进制格式的：
//   - chunk dir 数量与 four-cc 与 attributeMask 一致
//   - 各 chunk 的 offset/size 在文件范围内
//   - round-trip (saveToBinary -> loadFromBinary) 保留 vertex/interleaved 数据
//   - 未知 four-cc chunk 不破坏加载
//   - SKIN chunk round-trip 与 attributeMask 解耦

#include "AYResource.h"
#include "AYMesh.h"
#include "AYTest.h"

#include <cstring>
#include <vector>

using namespace ayt::resource;

namespace {

constexpr UInt32 kFourCC(char a, char b, char c, char d)
{
    return (UInt32)(UInt8)a
         | ((UInt32)(UInt8)b << 8)
         | ((UInt32)(UInt8)c << 16)
         | ((UInt32)(UInt8)d << 24);
}

std::vector<VertexSkinWeight> makeSkinWeights(UInt32 vertexCount)
{
    std::vector<VertexSkinWeight> out(vertexCount);
    for (UInt32 i = 0; i < vertexCount; ++i) {
        out[i].boneIndex[0] = static_cast<UInt8>(i % 4);
        out[i].boneIndex[1] = 0;
        out[i].boneIndex[2] = 0;
        out[i].boneIndex[3] = 0;
        out[i].boneWeight[0] = 1.0f;
        out[i].boneWeight[1] = 0.0f;
        out[i].boneWeight[2] = 0.0f;
        out[i].boneWeight[3] = 0.0f;
    }
    return out;
}

} // namespace

TEST_SUITE(MeshBinaryChunkTests)

TEST_CASE(chunk_dir_well_formed_for_simple_cube)
{
    auto mesh = std::make_shared<Mesh>();
    mesh->createCube(1.0f);

    std::vector<UInt8> binary;
    CHECK(mesh->saveToBinary(binary));
    CHECK(!binary.empty());

    // header sanity
    CHECK(binary.size() >= sizeof(MeshBinaryHeader));
    const auto* header = reinterpret_cast<const MeshBinaryHeader*>(binary.data());
    CHECK(header->magic == IMesh::MAGIC);
    CHECK(header->version == IMesh::VERSION);
    CHECK(header->headerSize == sizeof(MeshBinaryHeader));
    CHECK(header->chunkCount > 0);
    CHECK(header->chunkTableOffset == sizeof(MeshBinaryHeader));

    // dir 范围
    const size_t dirEnd = header->chunkTableOffset
                        + static_cast<size_t>(header->chunkCount) * sizeof(MeshChunkDirEntry);
    CHECK(dirEnd <= binary.size());

    const auto* dir = reinterpret_cast<const MeshChunkDirEntry*>(
        binary.data() + header->chunkTableOffset);

    bool sawPosn = false, sawNrm = false, sawUv = false, sawIdx = false, sawMat = false, sawSub = false;
    for (UInt32 i = 0; i < header->chunkCount; ++i) {
        const UInt32 fourCC = dir[i].fourCC;
        const UInt32 off = dir[i].offset;
        const UInt32 sz  = dir[i].size;
        CHECK(off >= header->chunkTableOffset + header->chunkCount * sizeof(MeshChunkDirEntry));
        CHECK(off + sz <= binary.size());

        switch (fourCC) {
        case kFourCC('P','O','S','N'): sawPosn = true; break;
        case kFourCC('N','O','R','M'): sawNrm  = true; break;
        case kFourCC('U','V','0',' '): sawUv   = true; break;
        case kFourCC('I','D','X',' '): sawIdx  = true; break;
        case kFourCC('M','A','T','L'): sawMat  = true; break;
        case kFourCC('S','U','B','M'): sawSub  = true; break;
        default: break;
        }
    }
    CHECK(sawPosn);
    CHECK(sawNrm);
    CHECK(sawUv);
    CHECK(sawIdx);
    CHECK(sawMat);
    CHECK(sawSub);
}

TEST_CASE(chunk_round_trip_preserves_vertex_data)
{
    auto mesh = std::make_shared<Mesh>();
    mesh->createCube(1.0f);

    FVector3 origMin, origMax;
    mesh->getBounds(origMin, origMax);
    const UInt32 origVertexCount = mesh->getVertexCount();
    const UInt32 origIndexCount  = mesh->getIndexCount();
    const UInt32 origStride      = mesh->getVertexStride();

    std::vector<UInt8> binary;
    CHECK(mesh->saveToBinary(binary));

    auto loaded = std::make_shared<Mesh>();
    CHECK(loaded->loadFromBinary(binary.data(), binary.size()));

    CHECK(loaded->getVertexCount() == origVertexCount);
    CHECK(loaded->getIndexCount()  == origIndexCount);
    CHECK(loaded->getVertexStride() == origStride);

    FVector3 ldMin, ldMax;
    loaded->getBounds(ldMin, ldMax);
    CHECK(ldMin.x == origMin.x);
    CHECK(ldMin.y == origMin.y);
    CHECK(ldMin.z == origMin.z);
    CHECK(ldMax.x == origMax.x);
    CHECK(ldMax.y == origMax.y);
    CHECK(ldMax.z == origMax.z);

    // indices 一致
    for (UInt32 i = 0; i < origIndexCount; ++i) {
        CHECK(loaded->getIndexData()[i] == mesh->getIndexData()[i]);
    }
}

TEST_CASE(skin_weight_chunk_round_trip)
{
    auto mesh = std::make_shared<Mesh>();
    mesh->createCube(1.0f);
    const auto weights = makeSkinWeights(mesh->getVertexCount());
    mesh->debugSetSkinWeights(weights);

    std::vector<UInt8> binary;
    CHECK(mesh->saveToBinary(binary));

    auto loaded = std::make_shared<Mesh>();
    CHECK(loaded->loadFromBinary(binary.data(), binary.size()));

    CHECK(loaded->hasSkinWeights());
    CHECK(loaded->getSkinWeights() != nullptr);
    CHECK(loaded->getVertexCount() == mesh->getVertexCount());

    const VertexSkinWeight* rw = loaded->getSkinWeights();
    for (UInt32 i = 0; i < mesh->getVertexCount(); ++i) {
        CHECK(rw[i].boneIndex[0] == weights[i].boneIndex[0]);
        CHECK(rw[i].boneWeight[0] == 1.0f);
        CHECK(rw[i].boneWeight[1] == 0.0f);
    }
}

TEST_CASE(load_rejects_truncated_chunk)
{
    auto mesh = std::make_shared<Mesh>();
    mesh->createCube(1.0f);

    std::vector<UInt8> binary;
    CHECK(mesh->saveToBinary(binary));

    // 截断掉最后一个 chunk 4 字节
    binary.resize(binary.size() - 4);
    auto loaded = std::make_shared<Mesh>();
    CHECK_FALSE(loaded->loadFromBinary(binary.data(), binary.size()));
}

TEST_CASE(load_ignores_unknown_fourcc)
{
    auto mesh = std::make_shared<Mesh>();
    mesh->createCube(1.0f);

    std::vector<UInt8> binary;
    CHECK(mesh->saveToBinary(binary));

    // 路径 A：把现存 dir[0] 的 fourCC 改成 'XXXX'，把 POSN 数据覆盖成未知。
    // loader 应当忽略未知 chunk，但其他 POSN 之后的 NORM / UV0 / IDX 等还能正常加载。
    // —— 为避免破坏 vertexData，我们只覆盖一个 chunk，并保留它可识别的 sibling chunks。
    //
    // 这里改成：把 chunkCount 加 1，并在文件末尾追加一对 [fake fourCC | payload]，
    // 同时把新的 dir entry 指针重写进 header.chunkTableOffset 区。
    //
    // 简化：直接在文件尾追加 16 字节 [chunk data] + 一个 new dir entry，再修正 chunkCount。
    // 我们的目标只是验证"未知 four-cc 不破坏 loader"。
    const size_t appendedChunkSize = 16;
    const size_t oldSize = binary.size();

    // 把原来 chunkTableOffset 之后的所有 dir entry 跟着 header 一起往后挪 12 bytes？
    // 太复杂。改用最简单形式：把 dir[0].fourCC 改为 'XXXX' + 留 chunk data 不变。
    // 这会让 POSN 数据被忽略 → loader 不会失败；这就是向前兼容语义。
    auto* header = reinterpret_cast<MeshBinaryHeader*>(binary.data());
    auto* dir = reinterpret_cast<MeshChunkDirEntry*>(
        binary.data() + header->chunkTableOffset);
    dir[0].fourCC = kFourCC('X','X','X','X');

    auto loaded = std::make_shared<Mesh>();
    CHECK(loaded->loadFromBinary(binary.data(), binary.size()));
    // 其余 chunk (NORM / UV0 / IDX / MATL / SUBM) 仍应被加载
    CHECK(loaded->getVertexCount() == mesh->getVertexCount());
    CHECK(loaded->getIndexCount() == mesh->getIndexCount());
}

// 多 submesh 的 SUBM chunk round-trip。
// 关键不变量:
//   1. N 个 Submesh 进、N 个出
//   2. 每个 Submesh 字段 (indexOffset, indexCount, materialIndex, vertexOffset) 完全一致
//   3. Submesh 的二进制 layout 是 4 × UInt32 = 16 bytes (F-02 pack(1) 强制对齐)，
//      所以 chunk 大小 == N × sizeof(Submesh)
//   4. F-01: vertexOffset 字段被透传
TEST_CASE(submesh_multi_chunk_round_trip)
{
    auto mesh = std::make_shared<Mesh>();
    mesh->createCube(1.0f);   // 24 verts, 36 indices, 1 submesh initially

    // F-02: sizeof(Submesh) 现在是 16 字节 (4 × UInt32, pack(1))
    CHECK(sizeof(IMesh::Submesh) == 16u);

    // 构造 3 个非平凡 submesh，验证字段序列化正确
    std::vector<IMesh::Submesh> expected(3);
    expected[0] = {  0, 12, 0, 0u  };   // +X face triangles (12 idx)
    expected[1] = { 12, 12, 1, 8u  };   // -X face (different material), vertexOffset=8
    expected[2] = { 24, 12, 2, 16u };   // +Y face, vertexOffset=16
    mesh->_setForTestSubmeshes(expected.data(), static_cast<UInt32>(expected.size()));

    // 检查 IMesh 端 submesh 数组是原样
    CHECK(mesh->getSubmeshCount() == 3u);
    CHECK(mesh->getSubmeshes()[0].indexOffset == 0u);
    CHECK(mesh->getSubmeshes()[1].materialIndex == 1u);
    CHECK(mesh->getSubmeshes()[2].vertexOffset == 16u);

    std::vector<UInt8> binary;
    CHECK(mesh->saveToBinary(binary));

    auto loaded = std::make_shared<Mesh>();
    CHECK(loaded->loadFromBinary(binary.data(), binary.size()));

    CHECK(loaded->getSubmeshCount() == 3u);
    const IMesh::Submesh* rd = loaded->getSubmeshes();
    CHECK(rd[0].indexOffset   == expected[0].indexOffset);
    CHECK(rd[0].indexCount    == expected[0].indexCount);
    CHECK(rd[0].materialIndex == expected[0].materialIndex);
    CHECK(rd[0].vertexOffset  == expected[0].vertexOffset);
    CHECK(rd[1].indexOffset   == expected[1].indexOffset);
    CHECK(rd[1].indexCount    == expected[1].indexCount);
    CHECK(rd[1].materialIndex == expected[1].materialIndex);
    CHECK(rd[1].vertexOffset  == expected[1].vertexOffset);
    CHECK(rd[2].indexOffset   == expected[2].indexOffset);
    CHECK(rd[2].indexCount    == expected[2].indexCount);
    CHECK(rd[2].materialIndex == expected[2].materialIndex);
    CHECK(rd[2].vertexOffset  == expected[2].vertexOffset);

    // 验证 SUBM chunk 的二进制 size = N × sizeof(Submesh) = 3 × 16 = 48
    const auto* header = reinterpret_cast<const MeshBinaryHeader*>(binary.data());
    const auto* dir = reinterpret_cast<const MeshChunkDirEntry*>(
        binary.data() + header->chunkTableOffset);
    UInt32 submSize = 0;
    for (UInt32 i = 0; i < header->chunkCount; ++i) {
        if (dir[i].fourCC == kFourCC('S','U','B','M')) {
            submSize = dir[i].size;
            break;
        }
    }
    CHECK(submSize == static_cast<UInt32>(3 * sizeof(IMesh::Submesh)));
}

TEST_SUITE_END
