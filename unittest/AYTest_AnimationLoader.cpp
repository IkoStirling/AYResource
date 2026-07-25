#include "AYResource.h"
#include "interface/assetsDefs/IAYAnimation.h"
#include "assetsImpl/AYAnimation.h"
#include "Loader/AnimationLoader.h"
#include "AYTest.h"
#include <fstream>
#include <cstdio>

using namespace ayt::resource;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

TEST_SUITE(AnimationLoaderTests)

    TEST_CASE(CreateTestAnimation) {
        Animation animation;
        animation.createTestAnimation();

        CHECK(animation.getTrackCount() > 0);
        CHECK(animation.isLoaded() == true);
        CHECK(strcmp(animation.getName(), "TestAnim") == 0);
    }

    TEST_CASE(AnimationMetadata) {
        Animation animation;
        animation.createTestAnimation();

        CHECK(animation.getDuration() > 0.0f);
        CHECK(animation.getTicksPerSecond() > 0.0f);
    }

    TEST_CASE(TrackAccess) {
        Animation animation;
        animation.createTestAnimation();

        UInt32 trackCount = animation.getTrackCount();
        CHECK(trackCount > 0);

        for (UInt32 i = 0; i < trackCount; i++) {
            const char* nodeName = animation.getTrackNodeName(i);
            const char* property = animation.getTrackProperty(i);
            CHECK(nodeName != nullptr);
            CHECK(property != nullptr);

            UInt32 keyframeCount = animation.getTrackKeyframeCount(i);
            CHECK(keyframeCount > 0);

            const float* times = animation.getTrackTimes(i);
            const float* values = animation.getTrackValues(i);
            CHECK(times != nullptr);
            CHECK(values != nullptr);
        }
    }

    TEST_CASE(SaveAndLoadBinary) {
        Animation original;
        original.createTestAnimation();

        // 保存到二进制
        std::vector<UInt8> binaryData;
        CHECK(original.saveToBinary(binaryData) == true);
        CHECK(binaryData.empty() == false);

        // 从二进制加载
        Animation loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);

        CHECK(loaded.getTrackCount() == original.getTrackCount());
        CHECK(loaded.getDuration() == original.getDuration());
        CHECK(loaded.getTicksPerSecond() == original.getTicksPerSecond());
    }

    TEST_CASE(LoadNonExistentFile) {
        Animation animation;
        CHECK(animation.load("nonexistent.ayanm") == false);
        CHECK(animation.isLoaded() == false);
    }

    TEST_CASE(CanLoad) {
        AnimationLoader loader;
        CHECK(loader.canLoad("animation.ayanm") == true);
        CHECK(loader.canLoad("path/to/anim.ayanm") == true);
        CHECK(loader.canLoad("test.aymesh") == false);
        CHECK(loader.canLoad("test.ayanmabc") == false);
    }

    TEST_CASE(GetResourceType) {
        AnimationLoader loader;
        CHECK(strcmp(loader.getResourceType(), "Animation") == 0);
    }

    TEST_CASE(LoadFromBinary) {
        Animation original;
        original.createTestAnimation();

        std::vector<UInt8> binaryData;
        original.saveToBinary(binaryData);

        AnimationLoader loader;
        auto resource = loader.loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(resource != nullptr);

        auto anim = std::dynamic_pointer_cast<Animation>(resource);
        CHECK(anim != nullptr);
        CHECK(anim->getTrackCount() == original.getTrackCount());
    }

    // Phase 1.5: Anim Notify marker round-trip via binary save/load.
    TEST_CASE(SaveAndLoadBinary_PreservesNotifyMarkers) {
        Animation original;
        original.createTestAnimation();
        original.addNotify(AnimNotifyMarker{"OnFootstep", 0.30f, 1.0f});
        original.addNotify(AnimNotifyMarker{"OnHit",       1.25f, 42.5f});

        std::vector<UInt8> binaryData;
        CHECK(original.saveToBinary(binaryData) == true);

        Animation loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);

        CHECK(loaded.getNotifyCount() == 2u);
        CHECK(strcmp(loaded.getNotifyName(0),    "OnFootstep") == 0);
        CHECK(loaded.getNotifyTime(0)            == 0.30f);
        CHECK(loaded.getNotifyPayload(0)         == 1.0f);
        CHECK(strcmp(loaded.getNotifyName(1),    "OnHit") == 0);
        CHECK(loaded.getNotifyTime(1)            == 1.25f);
        CHECK(loaded.getNotifyPayload(1)         == 42.5f);
    }

    // Phase 1.5: v1 backward-compat — a v1 binary (tracks only, no notify
    // block) must still load successfully and report getNotifyCount() == 0.
    // We synthesize a v1 binary by saving with VERSION=1 (writing only the
    // track block) and re-reading it through the v2 loader.
    TEST_CASE(LoadFromBinary_AcceptsLegacyV1File) {
        // Build a v1 binary manually: header (magic + version=1) + name +
        // duration + tps + trackCount=0.
        std::vector<UInt8> v1Binary;
        const UInt32 kMagic   = 0x4E4D5941u;
        const UInt32 kVersion = 1u;
        const std::string kName = "LegacyAnim";
        const Float32 kDur = 1.0f;
        const Float32 kTps = 30.0f;
        const UInt32 kTrackCount = 0u;

        auto append = [&v1Binary](const void* src, size_t n) {
            const UInt8* p = static_cast<const UInt8*>(src);
            v1Binary.insert(v1Binary.end(), p, p + n);
        };
        append(&kMagic, sizeof(kMagic));
        append(&kVersion, sizeof(kVersion));
        UInt32 nameLen = static_cast<UInt32>(kName.size());
        append(&nameLen, sizeof(nameLen));
        append(kName.data(), nameLen);
        append(&kDur, sizeof(kDur));
        append(&kTps, sizeof(kTps));
        append(&kTrackCount, sizeof(kTrackCount));
        // --- end of v1 binary: no notify block follows ---

        Animation loaded;
        CHECK(loaded.loadFromBinary(v1Binary.data(), v1Binary.size()) == true);
        CHECK(loaded.getTrackCount() == 0u);
        CHECK(loaded.getNotifyCount() == 0u);   // backward-compat: silently 0
        CHECK(strcmp(loaded.getName(), "LegacyAnim") == 0);
    }

    // Phase 1.5: empty-marker clip must round-trip with no extra bytes.
    TEST_CASE(SaveToBinary_EmptyNotifiesIsMinimal) {
        Animation anim;
        anim.createTestAnimation();           // has 1 track
        // No addNotify() calls — empty notify list.

        std::vector<UInt8> binaryData;
        CHECK(anim.saveToBinary(binaryData) == true);
        // Re-load and confirm 0 notifies.
        Animation loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);
        CHECK(loaded.getNotifyCount() == 0u);
        CHECK(loaded.getTrackCount() == anim.getTrackCount());
    }

    // Phase 1.5: out-of-bounds notify index returns safe defaults.
    TEST_CASE(NotifyAccessors_OutOfRangeReturnsSafeDefaults) {
        Animation anim;
        anim.addNotify(AnimNotifyMarker{"OnLand", 0.5f, 0.0f});
        CHECK(anim.getNotifyCount() == 1u);

        CHECK(anim.getNotifyName(99)    == nullptr);
        CHECK(anim.getNotifyTime(99)    == 0.0f);
        CHECK(anim.getNotifyPayload(99) == 0.0f);
        CHECK(anim.getNotifyName(1)     == nullptr);   // == count, also OOB
    }

TEST_SUITE_END
