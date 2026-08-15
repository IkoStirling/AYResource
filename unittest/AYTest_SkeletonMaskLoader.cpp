// AYTest_SkeletonMaskLoader.cpp — P3.x 刀 1 (2026-08-06) SkeletonMask loader
// tests.
//
// Covers the new AYResource asset class + loader surface:
//   - SkeletonMask authoring API (addEntry / clamp / duplicate-name / wildcard)
//   - .aymask v1 binary round-trip (saveToBinary → loadFromBinary → assert)
//   - SkeletonMaskLoader extension check + resource-type tag
//   - Forward-compat tripwire (version > VERSION rejected)
//   - Defensive checks (bad magic / null boneName / nonexistent file)
//
// Pattern mirrors AYTest_SkeletonLoader.cpp (8 cases) + authoring tests
// in line with AYTest_Skeleton.cpp.

#include "AYResource.h"
#include "AYResource/assetsDefs/ISkeletonMask.h"
#include "AYResource/assetsImpl/SkeletonMask.h"
#include "AYResource/Loader/SkeletonMaskLoader.h"
#include "AYTest.h"

#include <fstream>
#include <cstdio>

using namespace ayt::resource;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

TEST_SUITE(SkeletonMaskLoaderTests)

    // ===== Authoring API =====

    TEST_CASE(EmptyMask_NoEntriesNoWildcard) {
        SkeletonMask mask;
        mask.createTestMask();
        // createTestMask adds 4 entries: 3 named + 1 wildcard.
        // Override with a clean mask:
        mask.unload();
        CHECK(mask.getEntryCount() == 0u);
        CHECK(mask.getAuthoredBoneCount() == 0u);
        CHECK(mask.hasWildcard() == false);
    }

    TEST_CASE(AddNamedEntries_GetAuthoredBoneCount) {
        SkeletonMask mask;
        mask.addEntry("Spine", 0.5f);
        mask.addEntry("Head",  0.75f);
        mask.addEntry("L_Hand", 1.0f);
        CHECK(mask.getAuthoredBoneCount() == 3u);
        CHECK(mask.getEntryCount() == 3u);          // no wildcard
        CHECK(mask.hasWildcard() == false);
    }

    TEST_CASE(AddWildcard_HasWildcardTrue) {
        SkeletonMask mask;
        mask.addEntry("", 0.25f);
        CHECK(mask.hasWildcard() == true);
        CHECK(mask.wildcardWeight() == 0.25f);
        CHECK(mask.getEntryCount() == 1u);          // counts wildcard
        CHECK(mask.getAuthoredBoneCount() == 0u);   // but not as named
    }

    TEST_CASE(AddEntry_ClampToZeroAndOne) {
        SkeletonMask mask;
        mask.addEntry("Bone", -0.5f);
        CHECK(mask.getEntries()[0].weight == 0.0f);
        mask.addEntry("Bone", 2.0f);
        CHECK(mask.getEntries()[0].weight == 1.0f);
        mask.addEntry("Bone", 0.42f);
        CHECK(mask.getEntries()[0].weight == 0.42f);
    }

    TEST_CASE(DuplicateName_LastWins) {
        SkeletonMask mask;
        mask.addEntry("Bone", 0.3f);
        mask.addEntry("Bone", 0.7f);
        CHECK(mask.getAuthoredBoneCount() == 1u);
        CHECK(mask.getEntries()[0].weight == 0.7f);
    }

    // ===== Binary round-trip =====

    // ===== Binary round-trip =====

    // Mirror of the .aymask v1 header layout (in AYSkeletonMask.cpp).
    // Must match the on-disk structure byte-for-byte for the bad-magic /
    // version-rejection tests below.
#pragma pack(push, 1)
    struct SkeletonMaskHeaderV1Mirror {
        ayt::math::UInt32 magic;
        ayt::math::UInt16 version;
        ayt::math::FGuid  guid;
        ayt::math::UInt8  flags;
        ayt::math::UInt8  hasWildcard;
        float             wildcardWeight;
        ayt::math::UInt32 entryCount;
        ayt::math::UInt32 entryDataSize;
    };
#pragma pack(pop)
    static_assert(sizeof(SkeletonMaskHeaderV1Mirror) == 36,
                  ".aymask v1 mirror header size mismatch");

    TEST_CASE(SaveAndLoadBinary_RoundTrip) {
        SkeletonMask original;
        original.addEntry("Spine",    0.5f);
        original.addEntry("UpperArm", 0.0f);
        original.addEntry("", 0.25f);  // wildcard

        std::vector<ayt::math::UInt8> binaryData;
        CHECK(original.saveToBinary(binaryData) == true);
        CHECK(binaryData.empty() == false);
        CHECK(binaryData.size() > sizeof(SkeletonMaskHeaderV1Mirror) + 0);

        SkeletonMask loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);
        CHECK(loaded.getAuthoredBoneCount() == 2u);
        CHECK(loaded.hasWildcard() == true);
        CHECK(loaded.wildcardWeight() == 0.25f);
        // Named entries preserved.
        CHECK(loaded.getEntries()[0].name == "Spine");
        CHECK(loaded.getEntries()[0].weight == 0.5f);
        CHECK(loaded.getEntries()[1].name == "UpperArm");
        CHECK(loaded.getEntries()[1].weight == 0.0f);
    }

    TEST_CASE(LoadNonExistentFile_ReturnsFalse) {
        SkeletonMask mask;
        CHECK(mask.load("nonexistent_test_path_no_file.aymask") == false);
        CHECK(mask.isLoaded() == false);
    }

    // ===== Loader contract =====

    TEST_CASE(CanLoad_ByExtension) {
        SkeletonMaskLoader loader;
        CHECK(loader.canLoad("test.aymask") == true);
        CHECK(loader.canLoad("path/to/mask.aymask") == true);
        CHECK(loader.canLoad("test.ayskel") == false);
        CHECK(loader.canLoad("test.aymaskabc") == false);
        CHECK(loader.canLoad("short.ay") == false);
    }

    TEST_CASE(GetResourceType_IsSkeletonMask) {
        SkeletonMaskLoader loader;
        CHECK(std::strcmp(loader.getResourceType(), "SkeletonMask") == 0);
    }

    TEST_CASE(LoadFromBinary_RoundTrip) {
        SkeletonMask original;
        original.addEntry("Root", 1.0f);
        original.addEntry("Spine", 0.5f);

        std::vector<ayt::math::UInt8> binaryData;
        original.saveToBinary(binaryData);

        SkeletonMaskLoader loader;
        auto resource = loader.loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(resource != nullptr);

        auto mask = std::dynamic_pointer_cast<SkeletonMask>(resource);
        CHECK(mask != nullptr);
        CHECK(mask->getAuthoredBoneCount() == 2u);
        CHECK(mask->isLoaded() == true);
    }

    // ===== Forward-compat tripwire + defensive =====

    TEST_CASE(LoadFromBinary_RejectsVersionAbove1) {
        SkeletonMaskHeaderV1Mirror h{};
        h.magic          = 0x4D41534Bu;  // 'MASK'
        h.version        = 2;            // future version — should reject
        h.flags          = 0;
        h.hasWildcard    = 0;
        h.wildcardWeight = 0.0f;
        h.entryCount     = 0;
        h.entryDataSize  = 0;

        SkeletonMaskLoader loader;
        auto resource = loader.loadFromBinary(&h, sizeof(h));
        CHECK(resource == nullptr);
    }

    TEST_CASE(LoadFromBinary_DetectsBadMagic) {
        // Wrong magic — should reject.
        ayt::math::UInt8 badData[36] = {0};
        badData[0] = 0xDE;
        badData[1] = 0xAD;
        badData[2] = 0xBE;
        badData[3] = 0xEF;

        SkeletonMaskLoader loader;
        auto resource = loader.loadFromBinary(badData, sizeof(badData));
        CHECK(resource == nullptr);
    }

    TEST_CASE(AddEntry_NullPointer_NoOp) {
        SkeletonMask mask;
        mask.addEntry(nullptr, 0.5f);
        CHECK(mask.getEntryCount() == 0u);
        CHECK(mask.getAuthoredBoneCount() == 0u);
        CHECK(mask.hasWildcard() == false);
    }

    // ===== In-memory factory (preserved from P2.2 fixture) =====

    TEST_CASE(Create_InMemoryFactory) {
        auto mask = SkeletonMask::create();
        CHECK(mask != nullptr);
        CHECK(mask->isLoaded() == true);
        CHECK(mask->getType() == std::string("SkeletonMask"));
        mask->addEntry("Bone", 0.5f);
        CHECK(mask->getAuthoredBoneCount() == 1u);
    }

TEST_SUITE_END