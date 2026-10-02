#include "AYResource/Loader/VideoLoader.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYResource/IResourceLoader.h"
#include <AYIO/File.h>
#include <cstring>

namespace ayt::resource
{

// ===== VideoLoader =====

bool VideoLoader::canLoad(const std::string& path) const {
    return ayt::asset_format::matchesPath(path, ayt::asset_format::Id::Video);
}

std::shared_ptr<IResource> VideoLoader::load(const std::string& path) {
    auto video = std::make_shared<Video>();
    if (video->load(path)) {
        return video;
    }
    return nullptr;
}

std::shared_ptr<IResource> VideoLoader::loadFromBinary(const void* data, size_t size) {
    auto video = std::make_shared<Video>();
    if (video->loadFromBinary(data, size)) {
        return video;
    }
    return nullptr;
}

std::shared_ptr<IResource> VideoLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto video = load(path);
    if (callback) {
        callback(video);
    }
    return video;
}

} // namespace ayt::resource
