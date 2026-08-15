#include "AYResource/Loader/AudioLoader.h"
#include "AYResource/Converter/AudioDecoder.h"
#include "AYResource/IResourceLoader.h"
#include <AYMath/MathTypes.h>
#include <AYIO/File.h>
#include <cstring>
#include <cstdio>

namespace ayt::resource
{

namespace {

std::string basenameNoExt(const std::string& path)
{
    size_t lastSlash = path.find_last_of("/\\");
    size_t lastDot = path.find_last_of('.');
    if (lastDot != std::string::npos && (lastSlash == std::string::npos || lastDot > lastSlash)) {
        if (lastSlash != std::string::npos) {
            return path.substr(lastSlash + 1, lastDot - lastSlash - 1);
        }
        return path.substr(0, lastDot);
    }
    if (lastSlash != std::string::npos) {
        return path.substr(lastSlash + 1);
    }
    return path;
}

} // namespace

bool AudioLoader::canLoad(const std::string& path) const {
    const std::string ext = audioFileExtension(path);
    if (isCookedAudioExtension(ext)) {
        return true;
    }
#if defined(AY_AUDIO_LOOSE_FORMATS)
    return isLooseAudioExtension(ext);
#else
    (void)ext;
    return false;
#endif
}

std::shared_ptr<IResource> AudioLoader::load(const std::string& path) {
    auto audio = std::make_shared<Audio>();
    const std::string ext = audioFileExtension(path);

    if (isCookedAudioExtension(ext)) {
        if (audio->load(path)) {
            return audio;
        }
        return nullptr;
    }

#if defined(AY_AUDIO_LOOSE_FORMATS)
    if (!isLooseAudioExtension(ext)) {
        return nullptr;
    }
    PcmBuffer pcm;
    if (!decodeAudioFile(path, pcm) || pcm.empty()) {
        return nullptr;
    }
    audio->setName(basenameNoExt(path));
    audio->setFormat(pcm.sampleRate, pcm.channels, pcm.bitsPerSample, pcm.frameCount);
    audio->setData(std::move(pcm.bytes));
    audio->setLoaded(true);
    return audio;
#else
    (void)path;
    return nullptr;
#endif
}

std::shared_ptr<IResource> AudioLoader::loadFromBinary(const void* data, size_t size) {
    auto audio = std::make_shared<Audio>();
    if (audio->loadFromBinary(data, size)) {
        return audio;
    }
    return nullptr;
}

std::shared_ptr<IResource> AudioLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto audio = load(path);
    if (callback) {
        callback(audio);
    }
    return audio;
}

} // namespace ayt::resource
