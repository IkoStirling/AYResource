#include "AYResource/Loader/FontLoader.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYResource/IResourceLoader.h"
#include <AYIO/File.h>
#include <cstring>

namespace ayt::resource
{

// ===== FontLoader =====

bool FontLoader::canLoad(const std::string& path) const {
    return ayt::asset_format::matchesPath(path, ayt::asset_format::Id::Font);
}

std::shared_ptr<IResource> FontLoader::load(const std::string& path) {
    auto font = std::make_shared<FontAsset>();
    if (font->load(path)) {
        return font;
    }
    return nullptr;
}

std::shared_ptr<IResource> FontLoader::loadFromBinary(const void* data, size_t size) {
    auto font = std::make_shared<FontAsset>();
    if (font->loadFromBinary(data, size)) {
        return font;
    }
    return nullptr;
}

std::shared_ptr<IResource> FontLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto font = load(path);
    if (callback) {
        callback(font);
    }
    return font;
}

} // namespace ayt::resource
