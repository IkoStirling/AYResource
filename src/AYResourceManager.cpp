#include "AYResourceManager.h"
#include "AYResourceRegistry.h"
#include "AYResourceBootstrap.h"
#include "AYLooseDependency.h"
#include "AYMaterial.h"
#include "AYTexture.h"
#include "aystorage/IStorageDatabase.h"
#include "aystorage/IPackageReader.h"
#include <ayio/Path.h>
#include <cassert>
#include <cstdio>

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

    {
        std::lock_guard<std::mutex> lock(_paksMutex);
        auto it = _openedPaks.find(pakPath);
        if (it != _openedPaks.end()) {
            return it->second;
        }
    }

    auto reader = ayt::storage::IPackageReader::open(pakPath);
    if (!reader) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(_paksMutex);
    // Double-check after re-acquiring the lock: another thread may
    // have opened the same pak in between. Prefer the existing entry.
    auto it = _openedPaks.find(pakPath);
    if (it != _openedPaks.end()) {
        return it->second;
    }
    _openedPaks[pakPath] = std::move(reader);
    return _openedPaks[pakPath];
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
    for (const std::string& rawDep : collectLooseDependencies(filepath)) {
        const std::string depPath = normalizeResourcePath(rawDep);
        if (depPath.empty() || _cache.contains(depPath) || _loadingPaths.count(depPath) != 0) {
            continue;
        }
        if (!_loadInternal(depPath)) {
            (void)_installPlaceholder(depPath);
        }
    }
}

void ResourceManager::_loadIntrinsicDependencies(const std::string& filepath,
                                                 const IResource& resource) {
    for (const std::string& rawDep : collectIntrinsicDependencies(filepath, resource)) {
        const std::string depPath = normalizeResourcePath(rawDep);
        if (depPath.empty() || _cache.contains(depPath) || _loadingPaths.count(depPath) != 0) {
            continue;
        }
        if (!_loadInternal(depPath)) {
            std::fprintf(stderr,
                         "[ResourceManager] dependency load failed '%s' (owner '%s'); "
                         "installing placeholder\n",
                         depPath.c_str(), filepath.c_str());
            (void)_installPlaceholder(depPath);
        }
    }
}

void ResourceManager::_onResourceLoaded(const std::string& filepath,
                                        std::shared_ptr<IResource> resource) {
    _loadStates[filepath] = ResourceLoadState::Ready;
    if (resource) {
        _loadIntrinsicDependencies(filepath, *resource);
    }
}

std::shared_ptr<IResource> ResourceManager::_installPlaceholder(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    if (path.empty()) {
        return nullptr;
    }
    if (auto existing = _cache.get(path)) {
        _loadStates[path] = ResourceLoadState::Failed;
        return existing;
    }

    const std::string type = ResourceRegistry::getTypeFromPath(path);
    std::shared_ptr<IResource> placeholder;
    if (type == "Texture") {
        auto tex = std::make_shared<Texture>();
        // Magenta 1×1 — visible “missing texture” sentinel for L3 bind.
        tex->createSolidColor(1, 1, 255, 0, 255, 255);
        placeholder = tex;
    } else if (type == "Material") {
        auto mat = std::make_shared<Material>();
        mat->createDefault();
        placeholder = mat;
    } else {
        _loadStates[path] = ResourceLoadState::Failed;
        return nullptr;
    }

    _cache.put(path, placeholder);
    _resourceTypes[path] = type;
    _loadStates[path] = ResourceLoadState::Failed;
    return placeholder;
}

std::shared_ptr<IResource> ResourceManager::_loadInternal(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    if (path.empty()) {
        return nullptr;
    }
    if (auto existing = _cache.get(path)) {
        return existing;
    }

    // Cycle guard: a path currently mid-load must not re-enter.
    if (_loadingPaths.count(path) != 0) {
        return nullptr;
    }

    if (!areLoadersInitialized()) {
        initializeLoaders();
    }

    _loadingPaths.insert(path);
    _loadStates[path] = ResourceLoadState::Loading;

    std::shared_ptr<IResource> resource;

    if (_db && _db->isOpen()) {
        auto record = _db->getResource(path);
        if (record) {
            auto deps = _db->getLoadOrder(path);
            for (const auto& dep : deps) {
                if (!_cache.contains(dep) && _loadingPaths.count(dep) == 0) {
                    if (auto depRes = _loadFromDatabase(dep)) {
                        _loadStates[normalizeResourcePath(dep)] = ResourceLoadState::Ready;
                        (void)depRes;
                    } else {
                        (void)_installPlaceholder(dep);
                    }
                }
            }
            resource = _loadFromDatabase(path);
            if (resource) {
                _onResourceLoaded(path, resource);
            } else {
                _loadStates[path] = ResourceLoadState::Failed;
            }
            _loadingPaths.erase(path);
            return resource;
        }
    }

    _loadLooseDependencies(path);

    resource = ResourceRegistry::loadByPath(path);
    if (resource) {
        _cache.put(path, resource);
        const std::string type = ResourceRegistry::getTypeFromPath(path);
        if (!type.empty()) {
            _resourceTypes[path] = type;
        }
        _onResourceLoaded(path, resource);
    } else {
        _loadStates[path] = ResourceLoadState::Failed;
    }

    _loadingPaths.erase(path);
    return resource;
}

void ResourceManager::unloadResource(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    _cache.remove(path);
    _resourceTypes.erase(path);
    _loadStates.erase(path);
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
    _loadStates.clear();
    _loadingPaths.clear();
    {
        std::lock_guard<std::mutex> lock(_paksMutex);
        _openedPaks.clear();
    }
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

ResourceLoadState ResourceManager::getLoadState(const std::string& filepath) const {
    const std::string path = normalizeResourcePath(filepath);
    const auto it = _loadStates.find(path);
    if (it == _loadStates.end()) {
        return ResourceLoadState::NotLoaded;
    }
    return it->second;
}

bool ResourceManager::hasLoadFailed(const std::string& filepath) const {
    return getLoadState(filepath) == ResourceLoadState::Failed;
}

} // namespace ayt::resource