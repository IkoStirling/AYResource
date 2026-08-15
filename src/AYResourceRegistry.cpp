#include "AYResource/ResourceRegistry.h"
#include "AYResource/ResourceBootstrap.h"
#include <algorithm>

namespace ayt::resource
{

std::unordered_map<std::string, LoaderCreator>& ResourceRegistry::getLoaders() {
    static std::unordered_map<std::string, LoaderCreator> loaders;
    return loaders;
}

std::unordered_map<std::string, std::string>& ResourceRegistry::getExtensionMap() {
    static std::unordered_map<std::string, std::string> map;
    return map;
}

bool ResourceRegistry::registerLoader(const std::string& type, LoaderCreator creator) {
    getLoaders()[type] = creator;
    return true;
}

bool ResourceRegistry::unregisterLoader(const std::string& type) {
    return getLoaders().erase(type) > 0;
}

std::unique_ptr<IResourceLoader> ResourceRegistry::createLoader(const std::string& type) {
    auto it = getLoaders().find(type);
    if (it != getLoaders().end()) {
        return it->second();
    }
    return nullptr;
}

std::string ResourceRegistry::getTypeFromPath(const std::string& path) {
    auto ext = path.find_last_of('.');
    if (ext != std::string::npos) {
        std::string extension = path.substr(ext);
        return getTypeFromExtension(extension);
    }
    return {};
}

std::shared_ptr<IResource> ResourceRegistry::loadByPath(const std::string& path) {
    if (!areLoadersInitialized()) {
        initializeLoaders();
    }
    auto type = getTypeFromPath(path);
    if (type.empty()) {
        return nullptr;
    }
    auto loader = createLoader(type);
    if (!loader) {
        return nullptr;
    }
    return loader->load(path);
}

} // namespace ayt::resource