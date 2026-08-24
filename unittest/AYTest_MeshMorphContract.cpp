#include "AYResource/MeshMorphContract.h"
#include "AYResource/assetsDefs/IMesh.h"
#include "AYTest.h"

#include <cstring>
#include <limits>
#include <utility>
#include <vector>

using namespace ayt::resource;

namespace {

void writeU32(std::vector<UInt8>& out, UInt32 value)
{
    const size_t offset = out.size();
    out.resize(offset + sizeof(value));
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

void writeF32(std::vector<UInt8>& out, Float32 value)
{
    const size_t offset = out.size();
    out.resize(offset + sizeof(value));
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

void writeTarget(std::vector<UInt8>& out, UInt32 vertexIndex)
{
    const char name[] = "Smile";
    writeU32(out, static_cast<UInt32>(sizeof(name) - 1u));
    out.insert(out.end(), name, name + sizeof(name) - 1u);
    writeF32(out, 0.25f);
    writeU32(out, 1u);
    writeU32(out, kMeshMorphPayloadPosition | kMeshMorphPayloadNormal);
    writeU32(out, vertexIndex);
    writeF32(out, 1.0f); writeF32(out, 2.0f); writeF32(out, 3.0f);
    writeF32(out, 0.1f); writeF32(out, 0.2f); writeF32(out, 0.3f);
}

std::vector<UInt8> makePayload(bool versioned, UInt32 vertexIndex = 3u)
{
    std::vector<UInt8> out;
    if (versioned) {
        writeU32(out, kMeshMorphPayloadMagic);
        writeU32(out, kMeshMorphCurrentVersion);
        writeU32(out, 0u);
    }
    writeU32(out, 1u);
    writeTarget(out, vertexIndex);
    return out;
}

class MorphContractMesh final : public IMesh {
public:
    explicit MorphContractMesh(std::vector<UInt8> payload)
        : _payload(std::move(payload))
    {
        _extension = { kMeshMorphChunkType,
                       static_cast<UInt32>(_payload.size()),
                       _payload.data() };
    }

    bool load(const std::string&) override { return false; }
    bool unload() override { return true; }
    size_t sizeInBytes() const override { return _payload.size(); }

    UInt32 getVertexCount() const override { return 24u; }
    UInt32 getVertexStride() const override { return 0u; }
    const UInt8* getVertexData() const override { return nullptr; }
    UInt32 getIndexCount() const override { return 0u; }
    const UInt32* getIndexData() const override { return nullptr; }
    UInt8 getAttributeMask() const override { return 0u; }
    AttributeInfo getAttributeInfo(MeshAttribute) const override { return {}; }
    UInt32 getSubmeshCount() const override { return 0u; }
    const Submesh* getSubmeshes() const override { return nullptr; }
    UInt32 getMaterialSlotCount() const override { return 0u; }
    const char* getMaterialSlot(UInt32) const override { return nullptr; }
    Bounds getBounds() const override { return {}; }
    void getBounds(ayt::math::FVector3& min, ayt::math::FVector3& max) const override
    {
        min = {};
        max = {};
    }
    Bool hasBounds() const override { return false; }
    Bool hasSkinWeights() const override { return false; }
    const VertexSkinWeight* getSkinWeights() const override { return nullptr; }
    UInt32 getLODCount() const override { return 0u; }
    const LODData* getLODData() const override { return nullptr; }
    IMesh* getLOD(UInt32) override { return nullptr; }
    UInt32 getExtensionCount() const override { return 1u; }
    const Extension* getExtension(UInt32 index) const override
    {
        return index == 0u ? &_extension : nullptr;
    }
    const Extension* findExtension(UInt32 type) const override
    {
        return type == kMeshMorphChunkType ? &_extension : nullptr;
    }

private:
    std::vector<UInt8> _payload;
    Extension _extension{};
};

} // namespace

TEST_SUITE(MeshMorphContractTests)

TEST_CASE(versioned_contract_round_trips_complete_target)
{
    MorphContractMesh mesh(makePayload(true));
    MeshMorphContract contract;
    std::string error;
    CHECK(readMeshMorphContract(mesh, contract, &error));
    CHECK(error.empty());
    CHECK(contract.version == kMeshMorphCurrentVersion);
    CHECK(contract.targets.size() == 1u);
    CHECK(contract.targets[0].name == "Smile");
    CHECK(contract.targets[0].defaultWeight == 0.25f);
    CHECK(contract.targets[0].deltas.size() == 1u);
    CHECK(contract.targets[0].deltas[0].vertexIndex == 3u);
    CHECK(contract.targets[0].deltas[0].positionDelta[1] == 2.0f);
    CHECK(contract.targets[0].deltas[0].normalDelta[2] == 0.3f);

    MeshMorphContractSummary summary;
    CHECK(buildMeshMorphContractSummary(contract, summary));
    CHECK(summary.hasMorphTargets);
    CHECK(summary.targetCount == 1u);
    CHECK(summary.totalDeltaCount == 1u);
    CHECK(summary.morphVertexCount == 4u);
}

TEST_CASE(legacy_v1_payload_remains_readable)
{
    MorphContractMesh mesh(makePayload(false));
    MeshMorphContract contract;
    CHECK(readMeshMorphContract(mesh, contract));
    CHECK(contract.version == 1u);
    CHECK(contract.targets.size() == 1u);
    CHECK(contract.targets[0].name == "Smile");
}

TEST_CASE(contract_rejects_out_of_range_vertex_index)
{
    MorphContractMesh mesh(makePayload(true, 9999u));
    MeshMorphContract contract;
    std::string error;
    CHECK_FALSE(readMeshMorphContract(mesh, contract, &error));
    CHECK(!error.empty());
    CHECK(contract.targets.empty());
}

TEST_CASE(contract_rejects_count_that_cannot_fit_payload)
{
    std::vector<UInt8> payload;
    writeU32(payload, kMeshMorphPayloadMagic);
    writeU32(payload, kMeshMorphCurrentVersion);
    writeU32(payload, 0u);
    writeU32(payload, std::numeric_limits<UInt32>::max());
    MorphContractMesh mesh(std::move(payload));

    MeshMorphContract contract;
    CHECK_FALSE(readMeshMorphContract(mesh, contract));
    CHECK(contract.targets.empty());
}

TEST_CASE(contract_rejects_non_finite_delta)
{
    std::vector<UInt8> payload = makePayload(true);
    const Float32 nanValue = std::numeric_limits<Float32>::quiet_NaN();
    const size_t positionOffset = 16u + 4u + 5u + 4u + 4u + 4u + 4u;
    std::memcpy(payload.data() + positionOffset, &nanValue, sizeof(nanValue));
    MorphContractMesh mesh(std::move(payload));

    MeshMorphContract contract;
    CHECK_FALSE(readMeshMorphContract(mesh, contract));
}

TEST_SUITE_END
