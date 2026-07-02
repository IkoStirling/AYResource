#include "AYResource.h"
#include "interface/assetsDefs/IAYVideo.h"
#include "assetsImpl/AYVideo.h"
#include "Loader/VideoLoader.h"
#include "AYTest.h"
#include <fstream>
#include <cstdio>

using namespace ayt::resource;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

TEST_SUITE(VideoLoaderTests)

    TEST_CASE(CreateTestVideo) {
        Video video;
        video.createTestVideo(1280, 720, 30, 30.0f);

        CHECK(video.getWidth() == 1280);
        CHECK(video.getHeight() == 720);
        CHECK(video.getFrameCount() == 30);
        CHECK(video.getFrameRate() == 30.0f);
        CHECK(video.getDuration() > 0.0f);
        CHECK(video.getFrameSize() > 0);
        CHECK(video.isLoaded() == true);
    }

    TEST_CASE(FrameAccess) {
        Video video;
        video.createTestVideo(640, 480, 10, 30.0f);

        CHECK(video.getFrameCount() == 10);

        for (UInt32 i = 0; i < video.getFrameCount(); i++) {
            const UInt8* frameData = video.getFrameData(i);
            CHECK(frameData != nullptr);
        }

        // Out of bounds
        CHECK(video.getFrameData(video.getFrameCount()) == nullptr);
    }

    TEST_CASE(SaveAndLoadBinary) {
        Video original;
        original.createTestVideo(640, 480, 10, 30.0f);
        original.setName("TestVideo");

        // 保存到二进制
        std::vector<UInt8> binaryData;
        CHECK(original.saveToBinary(binaryData) == true);
        CHECK(binaryData.empty() == false);

        // 从二进制加载
        Video loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);

        CHECK(loaded.getWidth() == original.getWidth());
        CHECK(loaded.getHeight() == original.getHeight());
        CHECK(loaded.getFrameCount() == original.getFrameCount());
        CHECK(loaded.getFrameRate() == original.getFrameRate());
        CHECK(loaded.getFrameSize() == original.getFrameSize());
    }

    TEST_CASE(LoadNonExistentFile) {
        Video video;
        CHECK(video.load("nonexistent.ayvideo") == false);
        CHECK(video.isLoaded() == false);
    }

    TEST_CASE(CanLoad) {
        VideoLoader loader;
        CHECK(loader.canLoad("video.ayvideo") == true);
        CHECK(loader.canLoad("path/to/movie.ayvideo") == true);
        CHECK(loader.canLoad("test.aymesh") == false);
        CHECK(loader.canLoad("test.ayvideoabc") == false);
    }

    TEST_CASE(GetResourceType) {
        VideoLoader loader;
        CHECK(strcmp(loader.getResourceType(), "Video") == 0);
    }

    TEST_CASE(LoadFromBinary) {
        Video original;
        original.createTestVideo(640, 480, 10, 30.0f);

        std::vector<UInt8> binaryData;
        original.saveToBinary(binaryData);

        VideoLoader loader;
        auto resource = loader.loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(resource != nullptr);

        auto video = std::dynamic_pointer_cast<Video>(resource);
        CHECK(video != nullptr);
        CHECK(video->getFrameCount() == original.getFrameCount());
    }

TEST_SUITE_END
