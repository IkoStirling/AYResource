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

TEST_SUITE_END
