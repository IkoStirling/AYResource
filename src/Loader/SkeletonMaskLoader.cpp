#include "Loader\SkeletonMaskLoader.h"
#include "IAYResourceLoader.h"
#include <cstring>

namespace ayt::resource
{

SkeletonMaskLoader::SkeletonMaskLoader() = default;

bool SkeletonMaskLoader::canLoad(const std::string& path) const {
    // ".aymask" = 7 chars
    if (path.size() < 7) {
        return false;
    }
    return path.compare(path.size() - 7, 7, EXTENSION) == 0;
}

std::shared_ptr<IResource> SkeletonMaskLoader::load(const std::string& path) {
    auto mask = std::make_shared<SkeletonMask>();
    if (mask->load(path)) {
        return mask;
    }
    return nullptr;
}

std::shared_ptr<IResource> SkeletonMaskLoader::loadFromBinary(const void* data, size_t size) {
    auto mask = std::make_shared<SkeletonMask>();
    if (mask->loadFromBinary(data, size)) {
        return mask;
    }
    return nullptr;
}

std::shared_ptr<IResource> SkeletonMaskLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto mask = load(path);
    if (callback) {
        callback(mask);
    }
    return mask;
}

} // namespace ayt::resource