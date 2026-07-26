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

    // Phase 1.2 (P1.2): v2 backward-compat — a v2 .ayanm binary (tracks
    // followed by an optional notify block, but NO per-track blendMode byte
    // in the slot P1.2 inserts) must still load successfully. Every track's
    // blendMode field must default back to AnimBlendMode::Override (==0)
    // because the v2 file format does not carry the byte.
    //
    // We hand-craft a v2 binary with one track that, in v3 land, would
    // carry a blendMode byte. The v3 loader's `if (version >= 3)` gate must
    // skip the read for v2 files and leave the field at its struct default.
    TEST_CASE(LoadFromBinary_AcceptsLegacyV2File_AllTracksOverride) {
        const UInt32 kMagic   = 0x4E4D5941u;
        const UInt32 kVersion = 2u;
        const std::string kName  = "LegacyV2Additive";
        const std::string kBone  = "Root";
        const std::string kProp  = "position";
        const Float32 kDur = 2.0f;
        const Float32 kTps = 30.0f;
        const UInt32 kTrackCount = 1u;

        // v2 .ayanm format: header + name + dur + tps + track block (no
        // blendMode byte between valueType and timeCount) + notify count=0.
        // We deliberately do NOT append a blendMode byte here so the v3
        // loadFromBinary must prove it can skip reading it on version==2.
        std::vector<UInt8> v2Binary;
        auto append = [&v2Binary](const void* src, size_t n) {
            const UInt8* p = static_cast<const UInt8*>(src);
            v2Binary.insert(v2Binary.end(), p, p + n);
        };

        // header
        append(&kMagic, sizeof(kMagic));
        append(&kVersion, sizeof(kVersion));

        // name
        UInt32 nameLen = static_cast<UInt32>(kName.size());
        append(&nameLen, sizeof(nameLen));
        append(kName.data(), nameLen);

        // dur + tps
        append(&kDur, sizeof(kDur));
        append(&kTps, sizeof(kTps));

        // tracks
        append(&kTrackCount, sizeof(kTrackCount));
        // track 0: bone="Root", property="position", valueType=Vector3(0),
        //          1 keyframe (t=0, v=(1,2,3)) — smallest possible payload.
        UInt32 boneLen = static_cast<UInt32>(kBone.size());
        append(&boneLen, sizeof(boneLen));
        append(kBone.data(), boneLen);
        UInt32 propLen = static_cast<UInt32>(kProp.size());
        append(&propLen, sizeof(propLen));
        append(kProp.data(), propLen);
        UInt8 valueType = 0;  // AnimTrackType::Vector3
        append(&valueType, sizeof(valueType));
        // [intentionally no blendMode byte here — v2 didn't have it]
        UInt32 timeCount = 1u;
        append(&timeCount, sizeof(timeCount));
        Float32 t0 = 0.0f;
        append(&t0, sizeof(t0));
        UInt32 valueCount = 3u;
        append(&valueCount, sizeof(valueCount));
        Float32 v0[3] = {1.0f, 2.0f, 3.0f};
        append(v0, sizeof(v0));

        // notify block (v2 trailing block) — empty
        UInt32 notifyCount = 0u;
        append(&notifyCount, sizeof(notifyCount));

        Animation loaded;
        CHECK(loaded.loadFromBinary(v2Binary.data(), v2Binary.size()) == true);
        CHECK(loaded.getTrackCount() == 1u);
        CHECK(strcmp(loaded.getName(), "LegacyV2Additive") == 0);
        // The whole point of this test: every track's blendMode must be
        // Override (the default), because v2 had no byte for it.
        CHECK(loaded.getTrackBlendMode(0) == AnimBlendMode::Override);

        // Negative spot-check: an out-of-range index also returns Override
        // (the safe default the getter promises).
        CHECK(loaded.getTrackBlendMode(99u) == AnimBlendMode::Override);
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

    // P1.3 — forward-compat tripwire. loadFromBinary must reject any
    // version > IAnimation::VERSION (currently 4) so a future binary that
    // adds new bytes is caught loudly rather than silently mis-parsed
    // and writing corrupted skin matrices downstream.
    //
    // Hand-craft a 16-byte header (magic + version) + minimal name to
    // reach the version check; we don't need real track data — the
    // check happens BEFORE any track/notify reads.
    TEST_CASE(LoadFromBinary_RejectsVersionAbove4) {
        // Build a minimal valid-v4 buffer first, then mutate only the
        // version UInt32 to 5. The other bytes stay coherent so the
        // tripwire fires at the version guard, not at some later
        // length-mismatch guard.
        Animation ref;
        ref.createTestAnimation();
        std::vector<UInt8> good;
        CHECK(ref.saveToBinary(good) == true);
        CHECK(good.size() >= sizeof(UInt32) * 2);

        // Copy + patch version=5 (one above VERSION=4).
        std::vector<UInt8> bad = good;
        UInt32 futureVersion = 5u;
        std::memcpy(bad.data() + sizeof(UInt32), &futureVersion, sizeof(UInt32));

        Animation loaded;
        // The forward-compat guard returns false — caller can detect
        // a too-new binary instead of silently dropping or munging
        // track data.
        CHECK(loaded.loadFromBinary(bad.data(), bad.size()) == false);
        // A v4 binary (the current VERSION) must still load — pin
        // the non-tripwire path explicitly so a future regression
        // that broke both branches is caught.
        CHECK(ref.loadFromBinary(good.data(), good.size()) == true);
    }

TEST_SUITE_END
