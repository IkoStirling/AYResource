#include "AYResource/Loader/TilemapLoader.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYResource/IResourceLoader.h"
#include <AYIO/File.h>

namespace ayt::resource
{

// ===== TilemapLoader =====

bool TilemapLoader::canLoad(const std::string& path) const {
    return ayt::asset_format::matchesPath(path, ayt::asset_format::Id::Tilemap);
}

std::shared_ptr<IResource> TilemapLoader::load(const std::string& path) {
    auto tilemap = std::make_shared<TilemapAsset>();
    if (tilemap->load(path)) {
        return tilemap;
    }
    return nullptr;
}

std::shared_ptr<IResource> TilemapLoader::loadFromBinary(const void* data, size_t size) {
    auto tilemap = std::make_shared<TilemapAsset>();
    if (tilemap->loadFromBinary(data, size)) {
        return tilemap;
    }
    return nullptr;
}

std::shared_ptr<IResource> TilemapLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto tilemap = load(path);
    if (callback) {
        callback(tilemap);
    }
    return tilemap;
}

} // namespace ayt::resource
