#include "AYResource/Loader/ShaderLoader.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYResource/IResourceLoader.h"
#include <AYIO/File.h>
#include <cstring>
#include <cstdio>

namespace ayt::resource
{

// ===== ShaderLoader =====

bool ShaderLoader::canLoad(const std::string& path) const {
    return ayt::asset_format::matchesPath(path, ayt::asset_format::Id::ShaderBlob);
}

std::shared_ptr<IResource> ShaderLoader::load(const std::string& path) {
    auto shader = std::make_shared<Shader>();
    if (shader->load(path)) {
        return shader;
    }
    return nullptr;
}

std::shared_ptr<IResource> ShaderLoader::loadFromBinary(const void* data, size_t size) {
    auto shader = std::make_shared<Shader>();
    if (shader->loadFromBinary(data, size)) {
        return shader;
    }
    return nullptr;
}

std::shared_ptr<IResource> ShaderLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto shader = load(path);
    if (callback) {
        callback(shader);
    }
    return shader;
}

} // namespace ayt::resource
