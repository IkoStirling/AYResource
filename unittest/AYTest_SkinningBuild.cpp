#include "AYResource/SkinningBuild.h"
#include "AYResource/assetsDefs/IMesh.h"
#include "AYTest.h"

using namespace ayt::resource;

namespace {

MeshData makeSplitMesh()
{
    MeshData mesh;
    mesh.name = "generic_split_fixture";
    mesh.attributeMask = (1u << static_cast<UInt8>(MeshAttribute::Position))
                       | (1u << static_cast<UInt8>(MeshAttribute::SkinWeight));
    mesh.positions = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
    };
    mesh.indices = {0u, 1u, 2u, 0u, 2u, 3u};
    mesh.skinVertices.resize(4u);
    const UInt32 joints[] = {0u, 1u, 0u, 2u};
    for (UInt32 i = 0u; i < 4u; ++i) {
        mesh.skinVertices[i].joint[0] = joints[i];
        mesh.skinVertices[i].weight[0] = 1.0f;
    }
    SubmeshData section;
    section.indexCount = 6u;
    section.materialIndex = 7u;
    section.sourceMaterialIndex = 11u;
    mesh.submeshes.push_back(section);

    MorphTargetData morph;
    morph.name = "shared_vertex";
    MorphVertexDelta delta;
    delta.vertexIndex = 0u;
    delta.positionDelta[0] = 0.25f;
    morph.deltas.push_back(delta);
    mesh.morphTargets.push_back(morph);
    return mesh;
}

} // namespace

TEST_SUITE(SkinningBuildTests)

TEST_CASE(global_palette_is_compacted_without_splitting_when_it_fits)
{
    MeshData source = makeSplitMesh();
    MeshData cooked;
    SkinningBuildProfile profile;
    profile.maxBonesPerDraw = 3u;
    SkinningBuildStats stats;
    std::string error;

    CHECK(buildSkinnedRenderChunks(source, profile, cooked, &stats, &error));
    CHECK(error.empty());
    CHECK(cooked.skinIndexSpace == SkinIndexSpace::LocalPalette);
    CHECK(cooked.submeshes.size() == 1u);
    CHECK(cooked.submeshes[0].bonePalette.size() == 3u);
    CHECK(cooked.submeshes[0].bonePalette[0] == 0u);
    CHECK(cooked.submeshes[0].bonePalette[1] == 1u);
    CHECK(cooked.submeshes[0].bonePalette[2] == 2u);
    CHECK(cooked.skinVertices[3].joint[0] == 2u);
    CHECK(stats.splitSubmeshCount == 0u);
}

TEST_CASE(section_over_capacity_is_partitioned_on_triangle_boundaries)
{
    MeshData source = makeSplitMesh();
    MeshData cooked;
    SkinningBuildProfile profile;
    profile.maxBonesPerDraw = 2u;
    SkinningBuildStats stats;
    std::string error;

    CHECK(buildSkinnedRenderChunks(source, profile, cooked, &stats, &error));
    CHECK(cooked.submeshes.size() == 2u);
    CHECK(cooked.submeshes[0].indexCount == 3u);
    CHECK(cooked.submeshes[1].indexCount == 3u);
    CHECK(cooked.submeshes[0].materialIndex == 7u);
    CHECK(cooked.submeshes[1].sourceMaterialIndex == 11u);
    CHECK(cooked.submeshes[0].bonePalette.size() == 2u);
    CHECK(cooked.submeshes[1].bonePalette.size() == 2u);
    CHECK(cooked.positions.size() / 3u == 6u);
    CHECK(stats.splitSubmeshCount == 1u);
    CHECK(stats.duplicatedVertexCount == 2u);

    CHECK(cooked.morphTargets.size() == 1u);
    CHECK(cooked.morphTargets[0].deltas.size() == 2u);
    CHECK(cooked.morphTargets[0].deltas[0].positionDelta[0] == 0.25f);
    CHECK(cooked.morphTargets[0].deltas[1].positionDelta[0] == 0.25f);
}

TEST_CASE(profile_capacity_is_not_a_skeleton_size_limit)
{
    MeshData source = makeSplitMesh();
    source.skinVertices[3].joint[0] = 400u;
    MeshData cooked;
    SkinningBuildProfile profile;
    profile.maxBonesPerDraw = 2u;

    CHECK(buildSkinnedRenderChunks(source, profile, cooked));
    CHECK(cooked.submeshes[1].bonePalette[1] == 400u);
    CHECK(cooked.skinVertices.back().joint[0] == 1u);
}

TEST_CASE(pruned_influences_are_renormalized_after_palette_compaction)
{
    MeshData source = makeSplitMesh();
    source.skinVertices[0].joint[0] = 12u;
    source.skinVertices[0].weight[0] = 0.9995f;
    source.skinVertices[0].joint[1] = 900u;
    source.skinVertices[0].weight[1] = 0.0005f;

    MeshData cooked;
    SkinningBuildProfile profile;
    profile.maxBonesPerDraw = 4u;
    profile.minimumInfluence = 0.001f;

    CHECK(buildSkinnedRenderChunks(source, profile, cooked));
    CHECK(cooked.skinVertices[0].weight[0] == 1.0f);
    CHECK(cooked.skinVertices[0].weight[1] == 0.0f);
    CHECK(cooked.skinVertices[0].joint[1] == 0u);
    CHECK(cooked.submeshes[0].bonePalette.size() == 4u);
}

TEST_CASE(vertex_without_retained_influence_is_rejected)
{
    MeshData source = makeSplitMesh();
    for (UInt32 slot = 0u; slot < 4u; ++slot) {
        source.skinVertices[0].joint[slot] = slot;
        source.skinVertices[0].weight[slot] = 0.00025f;
    }

    MeshData cooked;
    SkinningBuildProfile profile;
    profile.minimumInfluence = 0.001f;
    std::string error;

    CHECK(!buildSkinnedRenderChunks(source, profile, cooked, nullptr, &error));
    CHECK(!error.empty());
}

TEST_SUITE_END
