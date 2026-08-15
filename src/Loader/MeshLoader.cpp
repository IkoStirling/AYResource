#include "AYResource/Loader/MeshLoader.h"
#include "AYResource/IResourceLoader.h"
#include "AYResource/assetsImpl/Mesh.h"
#include <AYMath/MathTypes.h>
#include <AYMath/MathUtils.h>
#include <AYIO/File.h>
#include <cstring>
#include <cstdio>

namespace ayt::resource
{
// ===== MeshLoader =====

bool MeshLoader::canLoad(const std::string& path) const {
    if (path.size() < 7) {
        return false;
    }
    return path.compare(path.size() - 7, 7, EXTENSION) == 0;
}

std::shared_ptr<IResource> MeshLoader::load(const std::string& path) {
    auto mesh = std::make_shared<Mesh>();
    if (mesh->load(path)) {
        return mesh;
    }
    return nullptr;
}

std::shared_ptr<IResource> MeshLoader::loadFromBinary(const void* data, size_t size) {
    auto mesh = std::make_shared<Mesh>();
    if (mesh->loadFromBinary(data, size)) {
        return mesh;
    }
    return nullptr;
}

std::shared_ptr<IResource> MeshLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto mesh = load(path);
    if (callback) {
        callback(mesh);
    }
    return mesh;
}

} // namespace ayt::resource