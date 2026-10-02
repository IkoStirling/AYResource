#include "AYResource/Loader/SkeletonMaskLoader.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYResource/IResourceLoader.h"
#include <cstring>

namespace ayt::resource
{

SkeletonMaskLoader::SkeletonMaskLoader() = default;

bool SkeletonMaskLoader::canLoad(const std::string& path) const {
    return ayt::asset_format::matchesPath(path, ayt::asset_format::Id::SkeletonMask);
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
