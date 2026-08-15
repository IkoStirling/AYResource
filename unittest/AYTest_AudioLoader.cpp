#include "AYResource.h"
#include "AYResource/assetsDefs/IAudio.h"
#include "AYResource/assetsImpl/Audio.h"
#include "AYResource/Loader/AudioLoader.h"
#include "AYTest.h"
#include <fstream>
#include <cstdio>

using namespace ayt::resource;

static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

TEST_SUITE(AudioLoaderTests)

    TEST_CASE(CreateSilence) {
        Audio audio;
        audio.createSilence(44100);

        CHECK(audio.getSampleRate() == 44100);
        CHECK(audio.getChannels() == 1);
        CHECK(audio.getBitsPerSample() == 16);
        CHECK(audio.getSampleCount() == 44100);
        CHECK(audio.getDataSize() > 0);
        CHECK(audio.isLoaded() == true);
    }

    TEST_CASE(CreateSineWave) {
        Audio audio;
        audio.createSineWave(440.0f, 1.0f);

        CHECK(audio.getSampleRate() == 44100);
        CHECK(audio.getChannels() == 1);
        CHECK(audio.getBitsPerSample() == 16);
        CHECK(audio.getSampleCount() == 44100);
        CHECK(audio.getDataSize() > 0);
    }

    TEST_CASE(SaveAndLoadBinary) {
        Audio original;
        original.createSilence(22050);
        original._name = "TestAudio";

        // 保存到二进制
        std::vector<UInt8> binaryData;
        CHECK(original.saveToBinary(binaryData) == true);
        CHECK(binaryData.empty() == false);

        // 从二进制加载
        Audio loaded;
        CHECK(loaded.loadFromBinary(binaryData.data(), binaryData.size()) == true);

        CHECK(loaded.getSampleRate() == original.getSampleRate());
        CHECK(loaded.getChannels() == original.getChannels());
        CHECK(loaded.getBitsPerSample() == original.getBitsPerSample());
        CHECK(loaded.getSampleCount() == original.getSampleCount());
        CHECK(loaded.getDataSize() == original.getDataSize());
    }

    TEST_CASE(LoadNonExistentFile) {
        Audio audio;
        CHECK(audio.load("nonexistent.ayaudio") == false);
        CHECK(audio.isLoaded() == false);
    }

    TEST_CASE(CanLoad) {
        AudioLoader loader;
        CHECK(loader.canLoad("sound.ayaudio") == true);
        CHECK(loader.canLoad("path/to/audio.ayaudio") == true);
        CHECK(loader.canLoad("test.aymesh") == false);
        CHECK(loader.canLoad("test.ayaudioabc") == false);
    }

    TEST_CASE(GetResourceType) {
        AudioLoader loader;
        CHECK(strcmp(loader.getResourceType(), "Audio") == 0);
    }

    TEST_CASE(LoadFromBinary) {
        Audio original;
        original.createSilence(22050);

        std::vector<UInt8> binaryData;
        original.saveToBinary(binaryData);

        AudioLoader loader;
        auto resource = loader.loadFromBinary(binaryData.data(), binaryData.size());
        CHECK(resource != nullptr);

        auto audio = std::dynamic_pointer_cast<Audio>(resource);
        CHECK(audio != nullptr);
        CHECK(audio->getSampleCount() == original.getSampleCount());
    }

TEST_SUITE_END
