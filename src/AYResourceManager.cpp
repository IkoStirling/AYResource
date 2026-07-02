#include "AYResourceManager.h"
#include "AYResourceRegistry.h"
#include "AYResourceBootstrap.h"
#include "AYLooseDependency.h"
#include "IAYStorageDatabase.h"
#include "IAYPackageReader.h"
#include <AYPath.h>
#include <cassert>

namespace ayt::resource
{

namespace {

std::string normalizeResourcePath(const std::string& path)
{
    return path.empty() ? path : ayt::io::path::normalize(path);
}

} // namespace

ResourceManager::ResourceManager()
    : _cache(ResourceCache::Config{})
    , _db(ayt::storage::IStorageDatabase::create(":memory:"))
    , _hotReloadWatcher(HotReloadWatcher{}) {
}

ResourceManager::~ResourceManager() = default;

void ResourceManager::setDatabase(std::unique_ptr<ayt::storage::IStorageDatabase> db) {
    _db = std::move(db);
}

ResourceManager& ResourceManager::instance() {
    static ResourceManager instance;
    return instance;
}

std::shared_ptr<ayt::storage::IPackageReader> ResourceManager::_getOrOpenPak(const std::string& pakPath) {
    if (pakPath.empty()) {
        return nullptr;
    }

    auto it = _openedPaks.find(pakPath);
    if (it != _openedPaks.end()) {
        return it->second;
    }

    auto reader = ayt::storage::IPackageReader::open(pakPath);
    if (!reader) {
        return nullptr;
    }

    _openedPaks[pakPath] = std::move(reader);
    return reader;
}

void ResourceManager::preloadResourcesWithTag(const std::string& tag, const std::string& category) {
    if (!_db || !_db->isOpen()) return;

    auto paths = _db->findByTag(tag, category);
    for (const auto& path : paths) {
        auto record = _db->getResource(path);
        if (record && !record->inPackage.empty()) {
            auto pkg = _getOrOpenPak(record->inPackage);
            if (pkg) {
                _loadFromDatabase(path);
            }
        } else {
            _loadInternal(path);
        }
    }
}

std::shared_ptr<IResource> ResourceManager::_loadFromDatabase(const std::string& filepath) {
    if (!_db || !_db->isOpen()) {
        return nullptr;
    }

    auto record = _db->getResource(filepath);
    if (!record) {
        return nullptr;
    }

    auto type = record->type;
    auto loader = ResourceRegistry::createLoader(type);
    if (!loader) {
        return nullptr;
    }

    std::shared_ptr<IResource> resource;
    if (!record->inPackage.empty()) {
        auto pkg = _getOrOpenPak(record->inPackage);
        if (!pkg) {
            return nullptr;
        }
        auto data = pkg->read(filepath);
        if (data.empty()) {
            return nullptr;
        }
        resource = loader->loadFromBinary(data.data(), data.size());
    } else {
        resource = loader->load(filepath);
    }

    if (resource) {
        _cache.putStrong(filepath, resource);
        _resourceTypes[filepath] = type;
    }
    return resource;
}

void ResourceManager::_loadLooseDependencies(const std::string& filepath) {
    for (const std::string& depPath : collectLooseDependencies(filepath)) {
        if (!_cache.contains(depPath)) {
            (void)_loadInternal(depPath);
        }
    }
}

std::shared_ptr<IResource> ResourceManager::_loadInternal(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    if (auto existing = _cache.get(path)) {
        return existing;
    }

    if (!areLoadersInitialized()) {
        initializeLoaders();
    }

    if (_db && _db->isOpen()) {
        auto record = _db->getResource(path);
        if (record) {
            auto deps = _db->getLoadOrder(path);
            for (const auto& dep : deps) {
                if (!_cache.contains(dep)) {
                    if (auto depRes = _loadFromDatabase(dep)) {
                        (void)depRes;
                    }
                }
            }
            return _loadFromDatabase(path);
        }
    }

    _loadLooseDependencies(path);

    auto resource = ResourceRegistry::loadByPath(path);
    if (resource) {
        _cache.put(path, resource);
        const std::string type = ResourceRegistry::getTypeFromPath(path);
        if (!type.empty()) {
            _resourceTypes[path] = type;
        }
    }
    return resource;
}

void ResourceManager::unloadResource(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    _cache.remove(path);
    _resourceTypes.erase(path);
}

void ResourceManager::reloadResource(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    if (auto resource = _cache.get(path)) {
        resource->reload(path);
    }
}

void ResourceManager::unloadAll() {
    _cache.clear();
    _resourceTypes.clear();
    _openedPaks.clear();
}

void ResourceManager::tagResource(const std::string& filepath, const ResourceTag& tag) {
    if (auto resource = _cache.get(filepath)) {
        resource->addTag(tag);
    }
}

void ResourceManager::untagResource(const std::string& filepath, const ResourceTag& tag) {
    if (auto resource = _cache.get(filepath)) {
        resource->removeTag(tag);
    }
}

void ResourceManager::unloadTagged(const ResourceTag& tag) {
    for (const auto& [path, resource] : _cache.getStrongCache()) {
        (void)path;
        if (resource->hasTag(tag)) {
            resource->unload();
        }
    }
}

void ResourceManager::unloadTaggedCategory(const std::string& category) {
    ResourceTag tag{"", category};
    for (const auto& [path, resource] : _cache.getStrongCache()) {
        (void)path;
        if (resource->hasTag(tag)) {
            resource->unload();
        }
    }
}

void ResourceManager::setCacheConfig(const ResourceCache::Config& config) {
    // 使用placement new重建_cache（因为mutex不可复制）
    _cache.~ResourceCache();
    new (&_cache) ResourceCache(config);
}

void ResourceManager::trimCache() {
    _cache.trimToConfig();
}

void ResourceManager::savePersistentCache(const std::string& path) {
    (void)path;
}

void ResourceManager::loadPersistentCache(const std::string& path) {
    (void)path;
}

void ResourceManager::watchResource(const std::string& filepath) {
    _hotReloadWatcher.watch(filepath);
}

void ResourceManager::unwatchResource(const std::string& filepath) {
    _hotReloadWatcher.unwatch(filepath);
}

void ResourceManager::setOnHotReload(HotReloadWatcher::FileChangeCallback callback) {
    _hotReloadWatcher.setOnFileChanged(callback);
}

void ResourceManager::update(float deltaTime) {
    _hotReloadWatcher.update();
    _asyncLoader.update(deltaTime);
}

size_t ResourceManager::getMemoryUsage() const {
    return _cache.memoryUsage();
}

size_t ResourceManager::getResourceCount() const {
    return _cache.strongCount();
}

bool ResourceManager::isLoaded(const std::string& filepath) const {
    const std::string path = normalizeResourcePath(filepath);
    auto r = _cache.get(path);
    return r && r->isLoaded();
}

std::shared_ptr<IResource> ResourceManager::getResource(const std::string& filepath) {
    return _cache.get(normalizeResourcePath(filepath));
}

} // namespace ayt::resource