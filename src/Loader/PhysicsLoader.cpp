#include "Loader\PhysicsLoader.h"
#include "IAYResourceLoader.h"
#include <AYFile.h>

namespace ayt::resource
{

// ===== PhysicsLoader =====

bool PhysicsLoader::canLoad(const std::string& path) const {
    if (path.size() < 7) {
        return false;
    }
    return path.compare(path.size() - 7, 7, EXTENSION) == 0;
}

std::shared_ptr<IResource> PhysicsLoader::load(const std::string& path) {
    auto physics = std::make_shared<Physics>();
    if (physics->load(path)) {
        return physics;
    }
    return nullptr;
}

std::shared_ptr<IResource> PhysicsLoader::loadFromBinary(const void* data, size_t size) {
    auto physics = std::make_shared<Physics>();
    if (physics->loadFromBinary(data, size)) {
        return physics;
    }
    return nullptr;
}

std::shared_ptr<IResource> PhysicsLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto physics = load(path);
    if (callback) {
        callback(physics);
    }
    return physics;
}

} // namespace ayt::resource