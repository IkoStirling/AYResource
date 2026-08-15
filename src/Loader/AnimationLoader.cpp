#include "AYResource/Loader/AnimationLoader.h"
#include <fstream>

namespace ayt::resource
{

AnimationLoader::AnimationLoader() = default;

bool AnimationLoader::canLoad(const std::string& path) const {
    if (path.size() < 6) return false;
    return path.compare(path.size() - 6, 6, EXTENSION) == 0;
}

std::shared_ptr<IResource> AnimationLoader::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return nullptr;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<UInt8> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    file.close();

    auto animation = std::make_shared<Animation>();
    if (!animation->loadFromBinary(data.data(), data.size())) {
        return nullptr;
    }

    return animation;
}

std::shared_ptr<IResource> AnimationLoader::loadFromBinary(const void* data, size_t size) {
    auto animation = std::make_shared<Animation>();
    if (!animation->loadFromBinary(data, size)) {
        return nullptr;
    }
    return animation;
}

std::shared_ptr<IResource> AnimationLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto animation = load(path);
    if (callback) {
        callback(animation);
    }
    return animation;
}

} // namespace ayt::resource