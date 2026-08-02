#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <thread>
#include <cstdint>
#include "IAYResource.h"
#include "AYResourceHandle.h"
#include "AYResourceCache.h"
#include "AYResourceRegistry.h"
#include "aystorage/IStorageDatabase.h"
#include "aystorage/IPackageReader.h"
#include "AYAsyncLoader.h"
#include "AYHotReloadWatcher.h"
#include <ayio/Path.h>

namespace ayt::resource
{

// P1: unified load lifecycle for Manager-owned paths.
// Failed may still have a placeholder in cache (magenta tex / default mat).
enum class ResourceLoadState : uint8_t {
    NotLoaded = 0,
    Loading = 1,
    Ready = 2,
    Failed = 3,
};

namespace detail {

template<typename T>
std::shared_future<std::shared_ptr<T>> castResourceFuture(
    std::shared_future<std::shared_ptr<IResource>> baseFuture)
{
    auto typedPromise = std::make_shared<std::promise<std::shared_ptr<T>>>();
    std::shared_future<std::shared_ptr<T>> typedFuture(typedPromise->get_future());

    std::thread([baseFuture = std::move(baseFuture), typedPromise]() mutable {
        typedPromise->set_value(std::dynamic_pointer_cast<T>(baseFuture.get()));
    }).detach();

    return typedFuture;
}

} // namespace detail

// ============================================================
// ResourceManager - 单例资源管理器
// ============================================================
class ResourceManager {
public:
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // ===== Singleton =====
    static ResourceManager& instance();

    // ===== Synchronous loading =====
    template<typename T, typename... Args>
    std::shared_ptr<T> load(const std::string& filepath, Args&&...) {
        auto resource = _loadInternal(filepath);
        return std::dynamic_pointer_cast<T>(resource);
    }

    // ===== Async loading with progress =====
    template<typename T, typename... Args>
    std::shared_future<std::shared_ptr<T>> loadAsync(
        const std::string& filepath,
        std::function<void(std::shared_ptr<T>)> callback = {},
        std::function<void(const std::string& path, float progress)> onProgress = {}
    ) {
        return detail::castResourceFuture<T>(_asyncLoader.loadAsync(
            filepath,
            [callback](std::shared_ptr<IResource> resource) {
                if (callback) {
                    callback(std::dynamic_pointer_cast<T>(resource));
                }
            },
            std::move(onProgress)));
    }

    // Legacy overload without progress (for backward compatibility)
    template<typename T, typename... Args>
    std::shared_future<std::shared_ptr<T>> loadAsync(
        const std::string& filepath,
        std::function<void(std::shared_ptr<T>)> callback
    ) {
        return loadAsync<T>(filepath, std::move(callback), nullptr);
    }

    // ===== Handle creation =====
    template<typename T>
    std::shared_ptr<ResourceHandle<T>> createHandle(const std::string& filepath);

    // ===== Unload/Reload =====
    void unloadResource(const std::string& filepath);
    void reloadResource(const std::string& filepath);
    void unloadAll();

    // ===== Tag management =====
    void tagResource(const std::string& filepath, const ResourceTag& tag);
    void untagResource(const std::string& filepath, const ResourceTag& tag);
    void unloadTagged(const ResourceTag& tag);
    void unloadTaggedCategory(const std::string& category);

    // ===== LRU management =====
    void setCacheConfig(const ResourceCache::Config& config);
    void trimCache();

    // ===== Persistent cache =====
    void savePersistentCache(const std::string& path);
    void loadPersistentCache(const std::string& path);

    // ===== Hot reload (P2) =====
    void watchResource(const std::string& filepath);
    void unwatchResource(const std::string& filepath);
    /// Invoked after L2 invalidate+reload for a changed path (L3 / game hooks).
    void setOnHotReload(HotReloadWatcher::FileChangeCallback callback);
    /// When true (default), successful loads auto-watch the loose file path.
    void setAutoWatchLoadedResources(bool enabled);
    bool autoWatchLoadedResources() const { return _autoWatchLoaded; }
    /// Hot-reload quiet window (default ~0.1s). Tests may set 0.
    void setHotReloadDebounceSeconds(float seconds);
    void setHotReloadPollIntervalSeconds(float seconds);

    // ===== Update loop =====
    void update(float deltaTime);

    // ===== Stats =====
    size_t getMemoryUsage() const;
    size_t getResourceCount() const;
    bool isLoaded(const std::string& filepath) const;
    std::shared_ptr<IResource> getResource(const std::string& filepath);

    // ===== P1 load state / placeholders =====
    ResourceLoadState getLoadState(const std::string& filepath) const;
    /// True when the path failed to load (placeholder may still be cached).
    bool hasLoadFailed(const std::string& filepath) const;

    // Expose cache for internal use
    ResourceCache& cache() { return _cache; }

    // Expose storage database (includes dependency management)
    ayt::storage::IStorageDatabase& storage() { return *_db; }

    // ===== Database injection (for testing) =====
    void setDatabase(std::unique_ptr<ayt::storage::IStorageDatabase> db);

    // ===== Package management =====
    /// @brief Preload resources with specific tag (e.g., "AlwaysLoaded")
    void preloadResourcesWithTag(const std::string& tag, const std::string& category = "");

private:
    ResourceManager();
    ~ResourceManager();

    friend class AsyncLoader;
    friend struct ResourceManagerTests;  // For unit tests

    std::shared_ptr<IResource> _loadInternal(const std::string& filepath);
    void _loadLooseDependencies(const std::string& filepath);
    void _loadIntrinsicDependencies(const std::string& filepath, const IResource& resource);
    void _onResourceLoaded(const std::string& filepath, std::shared_ptr<IResource> resource);
    std::shared_ptr<IResource> _installPlaceholder(const std::string& filepath);
    void _handleHotReload(const std::string& filepath);
    std::shared_ptr<ayt::storage::IPackageReader> _getOrOpenPak(const std::string& pakPath);
    std::shared_ptr<IResource> _loadFromDatabase(const std::string& filepath);

    ResourceCache _cache;
    std::unique_ptr<ayt::storage::IStorageDatabase> _db;
    // F1.4: _openedPaks and _resourceTypes were mutated without
    // synchronization. _paksMutex guards the pak-reader map; the
    // resource-types map's reads remain on the singleton's call
    // path (single-threaded for now) and gain the same lock only
    // when written from a load worker.
    mutable std::mutex _paksMutex;
    std::unordered_map<std::string, std::shared_ptr<ayt::storage::IPackageReader>> _openedPaks;
    AsyncLoader _asyncLoader;
    HotReloadWatcher _hotReloadWatcher;
    std::unordered_map<std::string, std::string> _resourceTypes;
    std::unordered_map<std::string, ResourceLoadState> _loadStates;
    // Guards recursive dep loads against cycles (mesh→mat→…→mesh).
    std::unordered_set<std::string> _loadingPaths;
    HotReloadWatcher::FileChangeCallback _userHotReloadCb;
    bool _autoWatchLoaded = true;
};

template<typename T>
std::shared_ptr<ResourceHandle<T>> ResourceManager::createHandle(const std::string& filepath) {
    // P1/P2: Normalize so handle path matches cache keys used by _loadInternal /
    // hot-reload eviction (otherwise Handle sticks to a stale duplicate entry).
    const std::string path =
        filepath.empty() ? filepath : ayt::io::path::normalize(filepath);
    auto loaderCb = [this](const std::string& p) -> std::shared_ptr<IResource> {
        return _loadInternal(p);
    };

    return std::make_shared<ResourceHandle<T>>(path, &_cache, std::move(loaderCb), true);
}

} // namespace ayt::resource
