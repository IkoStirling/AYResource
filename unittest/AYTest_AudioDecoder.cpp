#include "Converter/AudioDecoder.h"
#include "Converter/AudioConverter.h"
#include "Loader/AudioLoader.h"
#include "assetsImpl/AYAudio.h"
#include "IAYConverter.h"
#include "AYTest.h"

#include <ayio/File.h>
#include <ayio/Directory.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ayt::resource;

namespace {

std::string testDir()
{
    const char* env = std::getenv("AY_TEST_TMPDIR");
    if (env && env[0] != '\0') {
        return std::string(env) + "/audio_decoder";
    }
    return "audio_decoder_tmp";
}

bool ensureDir(const std::string& dir)
{
    return ayt::io::createDirectory(dir) || ayt::io::directoryExists(dir);
}

bool writeBytes(const std::string& path, const void* data, size_t n)
{
    return ayt::io::File::atomicWrite(path, data, n);
}

// Minimal mono 16-bit PCM WAV (440Hz-ish silence-ish square pulse), 100 frames.
std::vector<uint8_t> makeTinyWav(uint32_t sampleRate, uint16_t channels, uint32_t frames)
{
    const uint16_t bits = 16;
    const uint32_t dataBytes = frames * channels * (bits / 8);
    std::vector<uint8_t> pcm(dataBytes, 0);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t s = (i & 8) ? 8000 : -8000;
        for (uint16_t c = 0; c < channels; ++c) {
            const size_t off = (static_cast<size_t>(i) * channels + c) * 2;
            pcm[off] = static_cast<uint8_t>(s & 0xff);
            pcm[off + 1] = static_cast<uint8_t>((s >> 8) & 0xff);
        }
    }

    struct H {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t fileSize = 0;
        char wave[4] = {'W', 'A', 'V', 'E'};
        char fmt_[4] = {'f', 'm', 't', ' '};
        uint32_t fmtSize = 16;
        uint16_t audioFormat = 1;
        uint16_t numChannels = 0;
        uint32_t sampleRate = 0;
        uint32_t byteRate = 0;
        uint16_t blockAlign = 0;
        uint16_t bitsPerSample = 16;
        char data[4] = {'d', 'a', 't', 'a'};
        uint32_t dataSize = 0;
    } h;
    h.numChannels = channels;
    h.sampleRate = sampleRate;
    h.blockAlign = static_cast<uint16_t>(channels * (bits / 8));
    h.byteRate = sampleRate * h.blockAlign;
    h.dataSize = dataBytes;
    h.fileSize = 36 + dataBytes;

    std::vector<uint8_t> out(sizeof(H) + dataBytes);
    std::memcpy(out.data(), &h, sizeof(H));
    std::memcpy(out.data() + sizeof(H), pcm.data(), dataBytes);
    return out;
}

} // namespace

TEST_SUITE(AudioDecoderTests)

    TEST_CASE(ExtensionHelpers) {
        CHECK(audioFileExtension("a.WAV") == "wav");
        CHECK(isLooseAudioExtension("mp3"));
        CHECK(isLooseAudioExtension("ogg"));
        CHECK(isCookedAudioExtension("ayaudio"));
        CHECK(!isLooseAudioExtension("ayaudio"));
    }

    TEST_CASE(DecodeTinyWav) {
        const std::string dir = testDir();
        CHECK(ensureDir(dir));
        const std::string wavPath = dir + "/tone.wav";
        const auto wav = makeTinyWav(22050, 1, 100);
        CHECK(writeBytes(wavPath, wav.data(), wav.size()));

        PcmBuffer pcm;
        CHECK(decodeAudioFile(wavPath, pcm));
        CHECK(!pcm.empty());
        CHECK(pcm.sampleRate == 22050);
        CHECK(pcm.channels == 1);
        CHECK(pcm.bitsPerSample == 16);
        CHECK(pcm.frameCount == 100);
        CHECK(pcm.bytes.size() == 200);
    }

    TEST_CASE(CookWavToAyaudioRoundtrip) {
        const std::string dir = testDir();
        CHECK(ensureDir(dir));
        const std::string wavPath = dir + "/cook_src.wav";
        const auto wav = makeTinyWav(44100, 2, 64);
        CHECK(writeBytes(wavPath, wav.data(), wav.size()));

        AudioConverter conv(wavPath);
        conv.setOutputDir(dir);
        const ConversionResult r = conv.convert();
        CHECK(r.resources.size() == 1);
        CHECK(r.resources[0].type == std::string("Audio"));

        const std::string cooked = dir + "/audio/cook_src.ayaudio";
        CHECK(ayt::io::File::exists(cooked));

        AudioLoader loader;
        CHECK(loader.canLoad(cooked));
        auto res = loader.load(cooked);
        CHECK(res != nullptr);
        auto* audio = dynamic_cast<Audio*>(res.get());
        CHECK(audio != nullptr);
        CHECK(audio->getSampleRate() == 44100);
        CHECK(audio->getChannels() == 2);
        CHECK(audio->getBitsPerSample() == 16);
        CHECK(audio->getSampleCount() == 64);
        CHECK(audio->getDataSize() == 64 * 2 * 2);
    }

    TEST_CASE(IConverterCreateForLooseAudio) {
        auto c = IConverter::create("x.wav");
        CHECK(c != nullptr);
        CHECK(std::string(c->getSourceType()) == "Audio");
        c = IConverter::create("y.MP3");
        CHECK(c != nullptr);
        c = IConverter::create("z.ogg");
        CHECK(c != nullptr);
    }

    TEST_CASE(DecodeRejectsGarbage) {
        const std::string dir = testDir();
        CHECK(ensureDir(dir));
        const std::string badMp3 = dir + "/bad.mp3";
        const std::string badOgg = dir + "/bad.ogg";
        const char junk[] = "not an audio file!!!!";
        CHECK(writeBytes(badMp3, junk, sizeof(junk) - 1));
        CHECK(writeBytes(badOgg, junk, sizeof(junk) - 1));

        PcmBuffer pcm;
        CHECK(!decodeAudioFile(badMp3, pcm));
        CHECK(pcm.empty());
        CHECK(!decodeAudioFile(badOgg, pcm));
        CHECK(pcm.empty());
    }

#if defined(AY_AUDIO_LOOSE_FORMATS)
    TEST_CASE(LooseLoaderLoadsWav) {
        const std::string dir = testDir();
        CHECK(ensureDir(dir));
        const std::string wavPath = dir + "/loose.wav";
        const auto wav = makeTinyWav(16000, 1, 32);
        CHECK(writeBytes(wavPath, wav.data(), wav.size()));

        AudioLoader loader;
        CHECK(loader.canLoad(wavPath));
        auto res = loader.load(wavPath);
        CHECK(res != nullptr);
        auto* audio = dynamic_cast<Audio*>(res.get());
        CHECK(audio != nullptr);
        CHECK(audio->getSampleRate() == 16000);
        CHECK(audio->getSampleCount() == 32);
        CHECK(audio->isLoaded());
    }
#endif

TEST_SUITE_END
