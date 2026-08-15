#include "AYResourceManager.h"
#include "AYResourceRegistry.h"
#include "AYResourceBootstrap.h"
#include "AYLooseDependency.h"
#include "AYMaterial.h"
#include "AYTexture.h"
#include "AYStorage/IStorageDatabase.h"
#include "AYStorage/IPackageReader.h"
#include <AYIO/Path.h>
#include <AYIO/File.h>
#include <AYLog.h>
#include <cassert>
#include <cstdio>
#include <string>

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
    , _hotReloadWatcher()
{
    // P2: Manager owns L2 invalidate; user callback is post-reload notify.
    _hotReloadWatcher.setOnFileChanged([this](const std::string& path) {
        _handleHotReload(path);
    });
}

ResourceManager::~ResourceManager() = default;

void ResourceManager::setDatabase(std::unique_ptr<ayt::storage::IStorageDatabase> db) {
    _db = std::move(db);
    _dbBaseDir.clear();
}

bool ResourceManager::openDatabase(const std::string& dbPath) {
    if (dbPath.empty()) {
        return false;
    }
    auto db = ayt::storage::IStorageDatabase::open(dbPath);
    if (!db || !db->isOpen()) {
        return false;
    }
    _dbBaseDir = ayt::io::path::directory(normalizeResourcePath(dbPath));
    _db = std::move(db);
    return true;
}

ResourceManager& ResourceManager::instance() {
    static ResourceManager instance;
    return instance;
}

std::shared_ptr<ayt::storage::IPackageReader> ResourceManager::_getOrOpenPak(const std::string& pakPath) {
    if (pakPath.empty()) {
        return nullptr;
    }

    std::string resolved = pakPath;
    if (!ayt::io::path::isAbsolute(resolved) && !_dbBaseDir.empty()) {
        resolved = ayt::io::path::join(_dbBaseDir, resolved);
    }
    resolved = normalizeResourcePath(resolved);

    {
        std::lock_guard<std::mutex> lock(_paksMutex);
        auto it = _openedPaks.find(resolved);
        if (it != _openedPaks.end()) {
            if (_openPaksLru) {
                _openPaksLru->touch(resolved);
            }
            return it->second;
        }
    }

    auto reader = ayt::storage::IPackageReader::open(resolved);
    if (!reader) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(_paksMutex);
    // Double-check after re-acquiring the lock: another thread may
    // have opened the same pak in between. Prefer the existing entry.
    auto it = _openedPaks.find(resolved);
    if (it != _openedPaks.end()) {
        if (_openPaksLru) {
            _openPaksLru->touch(resolved);
        }
        return it->second;
    }

    // F3.3: cap the open-pak count via LRU eviction. When the cap is
    // reached, the next insertion drops the least-recently-touched
    // pak's reader (closes its file descriptor).
    if (_openPaksCap > 0 && _openedPaks.size() >= _openPaksCap) {
        if (!_openPaksLru) {
            _openPaksLru = std::make_unique<OpenPaksLru>();
        }
        const std::string evictPath = _openPaksLru->evictOldestExcept(resolved);
        if (!evictPath.empty()) {
            _openedPaks.erase(evictPath);
        }
    }
    if (_openPaksLru) {
        _openPaksLru->touch(resolved);
    }
    _openedPaks[resolved] = std::move(reader);
    return _openedPaks[resolved];
}

size_t ResourceManager::openPaksCount() const {
    std::lock_guard<std::mutex> lock(_paksMutex);
    return _openedPaks.size();
}

bool ResourceManager::invalidatePak(const std::string& pakPath) {
    std::lock_guard<std::mutex> lock(_paksMutex);
    if (pakPath.empty()) {
        const size_t n = _openedPaks.size();
        _openedPaks.clear();
        if (_openPaksLru) {
            _openPaksLru->clear();
        }
        return n > 0;
    }
    std::string resolved = pakPath;
    if (!ayt::io::path::isAbsolute(resolved) && !_dbBaseDir.empty()) {
        resolved = ayt::io::path::join(_dbBaseDir, resolved);
    }
    resolved = normalizeResourcePath(resolved);
    auto it = _openedPaks.find(resolved);
    if (it == _openedPaks.end()) {
        return false;
    }
    _openedPaks.erase(it);
    if (_openPaksLru) {
        _openPaksLru->forget(resolved);
    }
    return true;
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

    const std::string path = normalizeResourcePath(filepath);
    auto record = _db->getResource(path);
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
        auto data = pkg->read(path);
        if (data.empty()) {
            // Try forward-slash entry keys (cross-platform cook artifacts).
            std::string alt = path;
            for (char& c : alt) {
                if (c == '\\') {
                    c = '/';
                }
            }
            if (alt != path) {
                data = pkg->read(alt);
            }
        }
        if (data.empty()) {
            return nullptr;
        }
        resource = loader->loadFromBinary(data.data(), data.size());
    } else {
        resource = loader->load(path);
    }

    if (resource) {
        _cache.putStrong(path, resource);
        {
            std::lock_guard<std::mutex> lock(_loadsMutex);
            _resourceTypes[path] = type;
        }
    }
    return resource;
}

void ResourceManager::_loadLooseDependencies(const std::string& filepath) {
    for (const std::string& rawDep : collectLooseDependencies(filepath)) {
        const std::string depPath = normalizeResourcePath(rawDep);
        bool depInFlight = false;
        {
            std::lock_guard<std::mutex> lock(_loadsMutex);
            depInFlight = _loadingPaths.count(depPath) != 0;
        }
        if (depPath.empty() || _cache.contains(depPath) || depInFlight) {
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
        bool depInFlight = false;
        {
            std::lock_guard<std::mutex> lock(_loadsMutex);
            depInFlight = _loadingPaths.count(depPath) != 0;
        }
        if (depPath.empty() || _cache.contains(depPath) || depInFlight) {
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
    {
        std::lock_guard<std::mutex> lock(_loadsMutex);
        _loadStates[filepath] = ResourceLoadState::Ready;
    }
    if (resource) {
        _loadIntrinsicDependencies(filepath, *resource);
    }
    if (_autoWatchLoaded) {
        watchResource(filepath);
    }
}

void ResourceManager::_handleHotReload(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    if (path.empty()) {
        return;
    }

    std::fprintf(stderr, "[ResourceManager] hot-reload L2 '%s'\n", path.c_str());

    // Evict stale L2 instance (handles re-fetch via ResourceHandle::get).
    unloadResource(path);

    // Eager reload so dependents / L3 see fresh data on the same tick.
    std::shared_ptr<IResource> reloaded = _loadInternal(path);
    if (!reloaded) {
        (void)_installPlaceholder(path);
    }

    if (_userHotReloadCb) {
        _userHotReloadCb(path);
    }
}

std::shared_ptr<IResource> ResourceManager::_installPlaceholder(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    if (path.empty()) {
        return nullptr;
    }
    if (auto existing = _cache.get(path)) {
        {
            std::lock_guard<std::mutex> lock(_loadsMutex);
            _loadStates[path] = ResourceLoadState::Failed;
        }
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
        // F3.5: previously swallowed silently — game ran with a missing
        // placeholder and rendered as a black material forever. Now
        // routed through AYLog so cook + game both see it on the
        // ResourceManager channel.
        ayt::log::warn("[ResourceManager] _installPlaceholder no template for type '%s' (path '%s')",
                       type.c_str(), path.c_str());
        {
            std::lock_guard<std::mutex> lock(_loadsMutex);
            _loadStates[path] = ResourceLoadState::Failed;
        }
        return nullptr;
    }

    _cache.put(path, placeholder);
    {
        std::lock_guard<std::mutex> lock(_loadsMutex);
        _resourceTypes[path] = type;
        _loadStates[path] = ResourceLoadState::Failed;
    }
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
    {
        std::lock_guard<std::mutex> lock(_loadsMutex);
        if (_loadingPaths.count(path) != 0) {
            return nullptr;
        }
        _loadingPaths.insert(path);
        _loadStates[path] = ResourceLoadState::Loading;
    }

    if (!areLoadersInitialized()) {
        initializeLoaders();
    }

    std::shared_ptr<IResource> resource;

    if (_db && _db->isOpen()) {
        auto record = _db->getResource(path);
        if (record) {
            auto deps = _db->getLoadOrder(path);
            for (const auto& dep : deps) {
                const std::string depPath = normalizeResourcePath(dep);
                bool depInFlight = false;
                {
                    std::lock_guard<std::mutex> lock(_loadsMutex);
                    depInFlight = _loadingPaths.count(depPath) != 0;
                }
                if (!_cache.contains(depPath) && !depInFlight) {
                    if (auto depRes = _loadFromDatabase(depPath)) {
                        {
                            std::lock_guard<std::mutex> lock(_loadsMutex);
                            _loadStates[depPath] = ResourceLoadState::Ready;
                        }
                        (void)depRes;
                    } else if (auto looseDep = _loadInternal(depPath)) {
                        (void)looseDep;
                    } else {
                        (void)_installPlaceholder(depPath);
                    }
                }
            }
            resource = _loadFromDatabase(path);
            if (resource) {
                _onResourceLoaded(path, resource);
                {
                    std::lock_guard<std::mutex> lock(_loadsMutex);
                    _loadingPaths.erase(path);
                }
                return resource;
            }
            // P4: DB/pak miss → fall through to loose disk path.
            std::fprintf(stderr,
                         "[ResourceManager] DB/pak load failed '%s'; trying loose file\n",
                         path.c_str());
        }
    }

    _loadLooseDependencies(path);

    resource = ResourceRegistry::loadByPath(path);
    if (resource) {
        _cache.put(path, resource);
        const std::string type = ResourceRegistry::getTypeFromPath(path);
        if (!type.empty()) {
            {
                std::lock_guard<std::mutex> lock(_loadsMutex);
                _resourceTypes[path] = type;
            }
        }
        _onResourceLoaded(path, resource);
    } else {
        {
            std::lock_guard<std::mutex> lock(_loadsMutex);
            _loadStates[path] = ResourceLoadState::Failed;
        }
    }

    {
        std::lock_guard<std::mutex> lock(_loadsMutex);
        _loadingPaths.erase(path);
    }
    return resource;
}

void ResourceManager::unloadResource(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    _cache.remove(path);
    {
        std::lock_guard<std::mutex> lock(_loadsMutex);
        _resourceTypes.erase(path);
        _loadStates.erase(path);
    }
}

void ResourceManager::reloadResource(const std::string& filepath) {
    const std::string path = normalizeResourcePath(filepath);
    if (auto resource = _cache.get(path)) {
        resource->reload(path);
    }
}

void ResourceManager::unloadAll() {
    _hotReloadWatcher.unwatchAll();
    _cache.clear();
    {
        std::lock_guard<std::mutex> lock(_loadsMutex);
        _resourceTypes.clear();
        _loadStates.clear();
        _loadingPaths.clear();
    }
    {
        std::lock_guard<std::mutex> lock(_paksMutex);
        _openedPaks.clear();
    }
    // Keep _db / _dbBaseDir — ship mount survives unloadAll (tests reset via setDatabase).
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
    // F3.4: previously did placement-new on the non-trivially-destructible
    // member, which is UB if any other thread is reading the cache at
    // the moment the destructor runs. Now delegated to ResourceCache::rebuild
    // which holds the cache's internal mutex for the full reset.
    _cache.rebuild(config);
}

void ResourceManager::trimCache() {
    _cache.trimToConfig();
}

void ResourceManager::savePersistentCache(const std::string& path) {
    if (path.empty()) {
        return;
    }
    // P3: persist a lightweight residency index (paths already on disk/pak).
    // Format: line-oriented, versioned.
    //   AYCACHE 1
    //   <path>\t<type>\t<sizeInBytes>
    std::string out = "AYCACHE 1\n";
    for (const auto& entry : _cache.snapshotStrong()) {
        out += entry.path;
        out += '\t';
        out += entry.type;
        out += '\t';
        out += std::to_string(entry.sizeInBytes);
        out += '\n';
    }
    (void)ayt::io::File::writeAllText(path, out);
}

void ResourceManager::loadPersistentCache(const std::string& path) {
    if (path.empty() || !ayt::io::File::exists(path)) {
        return;
    }
    const std::string text = ayt::io::File::readAllText(path);
    if (text.size() < 8 || text.compare(0, 8, "AYCACHE ") != 0) {
        return;
    }

    // Disable auto-watch during bulk preload to avoid watch storms.
    const bool prevWatch = _autoWatchLoaded;
    _autoWatchLoaded = false;

    size_t pos = 0;
    // Skip header line.
    const size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) {
        _autoWatchLoaded = prevWatch;
        return;
    }
    pos = nl + 1;

    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) {
            continue;
        }
        const std::string assetPath = line.substr(0, t1);
        if (!assetPath.empty()) {
            (void)_loadInternal(assetPath);
        }
    }

    _autoWatchLoaded = prevWatch;
}

void ResourceManager::watchResource(const std::string& filepath) {
    _hotReloadWatcher.watch(normalizeResourcePath(filepath));
}

void ResourceManager::unwatchResource(const std::string& filepath) {
    _hotReloadWatcher.unwatch(normalizeResourcePath(filepath));
}

void ResourceManager::setOnHotReload(HotReloadWatcher::FileChangeCallback callback) {
    _userHotReloadCb = std::move(callback);
}

void ResourceManager::setAutoWatchLoadedResources(bool enabled) {
    _autoWatchLoaded = enabled;
}

void ResourceManager::setHotReloadDebounceSeconds(float seconds) {
    _hotReloadWatcher.setDebounceSeconds(seconds);
}

void ResourceManager::setHotReloadPollIntervalSeconds(float seconds) {
    _hotReloadWatcher.setPollInterval(seconds);
}

void ResourceManager::update(float deltaTime) {
    _hotReloadWatcher.update();
    _asyncLoader.update(deltaTime);
    _cache.tick(deltaTime);
}

size_t ResourceManager::getMemoryUsage() const {
    return _cache.memoryUsage();
}

size_t ResourceManager::getResourceCount() const {
    return _cache.strongCount();
}

CacheStats ResourceManager::getCacheStats() const {
    return _cache.stats();
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
    std::lock_guard<std::mutex> lock(_loadsMutex);
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