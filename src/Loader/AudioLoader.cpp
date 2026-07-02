#include "Loader\AudioLoader.h"
#include "IAYResourceLoader.h"
#include <AYMathTypes.h>
#include <AYFile.h>
#include <cstring>
#include <cstdio>

namespace ayt::resource
{

// ===== AudioLoader =====

bool AudioLoader::canLoad(const std::string& path) const {
    if (path.size() < 8) { // ".ayaudio" = 8 chars
        return false;
    }
    return path.compare(path.size() - 8, 8, EXTENSION) == 0;
}

std::shared_ptr<IResource> AudioLoader::load(const std::string& path) {
    auto audio = std::make_shared<Audio>();
    if (audio->load(path)) {
        return audio;
    }
    return nullptr;
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