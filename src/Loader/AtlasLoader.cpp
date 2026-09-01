#include "AYResource/Loader/AtlasLoader.h"

#include "AYResource/assetsImpl/AtlasAsset.h"

namespace ayt::resource
{

bool AtlasLoader::canLoad(const std::string& path) const
{
    constexpr const char* extension = ".ayatlas";
    constexpr size_t length = 8u;
    return path.size() >= length
        && path.compare(path.size() - length, length, extension) == 0;
}

std::shared_ptr<IResource> AtlasLoader::load(const std::string& path)
{
    auto atlas = std::make_shared<AtlasAsset>();
    return atlas->load(path) ? atlas : nullptr;
}

std::shared_ptr<IResource> AtlasLoader::loadFromBinary(
    const void* data, size_t size)
{
    auto atlas = std::make_shared<AtlasAsset>();
    return atlas->loadFromBinary(data, size) ? atlas : nullptr;
}

std::shared_ptr<IResource> AtlasLoader::loadAsync(
    const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback)
{
    auto atlas = load(path);
    if (callback) callback(atlas);
    return atlas;
}

} // namespace ayt::resource
