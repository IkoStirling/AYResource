#include "AYResource/Loader/SkeletonLoader.h"
#include "AYResource/IResourceLoader.h"
#include <cstring>

namespace ayt::resource
{

SkeletonLoader::SkeletonLoader() = default;

bool SkeletonLoader::canLoad(const std::string& path) const {
    if (path.size() < 7) { // ".ayskel" = 7 chars
        return false;
    }
    return path.compare(path.size() - 7, 7, EXTENSION) == 0;
}

std::shared_ptr<IResource> SkeletonLoader::load(const std::string& path) {
    auto skeleton = std::make_shared<Skeleton>();
    if (skeleton->load(path)) {
        return skeleton;
    }
    return nullptr;
}

std::shared_ptr<IResource> SkeletonLoader::loadFromBinary(const void* data, size_t size) {
    auto skeleton = std::make_shared<Skeleton>();
    if (skeleton->loadFromBinary(data, size)) {
        return skeleton;
    }
    return nullptr;
}

std::shared_ptr<IResource> SkeletonLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto skeleton = load(path);
    if (callback) {
        callback(skeleton);
    }
    return skeleton;
}

} // namespace ayt::resource