#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <future>
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

// F3.3: small LRU for the open-pak reader map. Reset on every touch()
// and pops the oldest entry when evictOldestExcept() is called. Tracks
// only ordering — the actual lookup is still _openedPaks.find().
class OpenPaksLru {
public:
    void touch(const std::string& key) {
        auto it = std::find(_order.begin(), _order.end(), key);
        if (it != _order.end()) {
            _order.erase(it);
        }
        _order.push_back(key);
    }

    void forget(const std::string& key) {
        auto it = std::find(_order.begin(), _order.end(), key);
        if (it != _order.end()) {
            _order.erase(it);
        }
    }

    std::string evictOldestExcept(const std::string& keep) {
        for (auto it = _order.begin(); it != _order.end(); ++it) {
            if (*it != keep) {
                const std::string victim = *it;
                _order.erase(it);
                return victim;
            }
        }
        return {};
    }

    void clear() { _order.clear(); }

private:
    std::vector<std::string> _order;
};

// P1: unified load lifecycle for Manager-owned paths.
// Failed may still have a placeholder in cache (magenta tex / default mat).
enum class ResourceLoadState : uint8_t {
    NotLoaded = 0,
    Loading = 1,
    Ready = 2,
    Failed = 3,
};

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

    // ===== Async loading with progress (P3: no extra cast thread) =====
    template<typename T, typename... Args>
    std::shared_future<std::shared_ptr<T>> loadAsync(
        const std::string& filepath,
        std::function<void(std::shared_ptr<T>)> callback = {},
        std::function<void(const std::string& path, float progress)> onProgress = {}
    ) {
        auto typedPromise = std::make_shared<std::promise<std::shared_ptr<T>>>();
        std::shared_future<std::shared_ptr<T>> typedFuture(typedPromise->get_future());

        (void)_asyncLoader.loadAsync(
            filepath,
            [callback, typedPromise](std::shared_ptr<IResource> resource) {
                auto typed = std::dynamic_pointer_cast<T>(resource);
                try {
                    typedPromise->set_value(typed);
                } catch (const std::future_error&) {
                }
                if (callback) {
                    callback(typed);
                }
            },
            std::move(onProgress));

        return typedFuture;
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
    // L2-only: removes cache residency. Does NOT notify L3 / setOnHotReload.
    // GPU handles remain valid until Renderer destroy* or hot-reload refresh.
    // See docs/ownership-contracts.md (P6).
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
    /// Demote under budget; does NOT notify L3 (same contract as unload*).
    void trimCache();

    // ===== Persistent cache =====
    void savePersistentCache(const std::string& path);
    void loadPersistentCache(const std::string& path);

    // ===== Hot reload (P2) =====
    void watchResource(const std::string& filepath);
    void unwatchResource(const std::string& filepath);
    /// Invoked after L2 invalidate+reload for a changed path (L3 / game hooks).
    /// This is the only automatic L2→L3 sync path (Renderer: onResourceFileChanged).
    /// Not called from unload*/trimCache.
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
    CacheStats getCacheStats() const;

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

    // ===== P4 ship mount =====
    /// Open a cooked resources.db and prefer DB/pak loads. Pak paths in
    /// `in_package` that are relative are resolved against the DB directory.
    bool openDatabase(const std::string& dbPath);

    // ===== Package management =====
    /// @brief Preload resources with specific tag (e.g., "AlwaysLoaded")
    void preloadResourcesWithTag(const std::string& tag, const std::string& category = "");

    // ===== F3.3: cap + invalidate pak reader cache =====
    /// Drop a single pak reader from the LRU cache (file descriptor
    /// released). Safe to call while other loads are in flight; the
    /// next _getOrOpenPak will reopen. Pass empty string to drop all.
    /// Returns true if a reader was actually removed.
    bool invalidatePak(const std::string& pakPath = {});
    /// Cap the open-pak count. 0 = unlimited (legacy default).
    /// When the cap is exceeded, the least-recently-touched pak is
    /// closed (the underlying IPackageReader's destructor closes the fd).
    void setOpenPaksCap(size_t cap) { _openPaksCap = cap; }
    size_t openPaksCount() const;

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
    // CM-5 (2026-08-12): guards _loadingPaths / _loadStates / _resourceTypes.
    // The F1.4 comment above promised these maps "gain the same lock when
    // written from a load worker" — the lock was never applied, and the
    // async pool runs _loadInternal on N threads concurrently, so the
    // unlocked mutations were a data race (heap corruption surfacing as
    // crashes in unrelated later tests). Lock scopes are tiny — each map
    // op only; never hold across an actual load (which would serialize
    // the async pool).
    mutable std::mutex _loadsMutex;
    // F3.3: cap + LRU for the pak reader map. 0 = unlimited.
    size_t _openPaksCap = 0;
    std::unique_ptr<OpenPaksLru> _openPaksLru;
    AsyncLoader _asyncLoader;
    HotReloadWatcher _hotReloadWatcher;
    std::unordered_map<std::string, std::string> _resourceTypes;
    std::unordered_map<std::string, ResourceLoadState> _loadStates;
    // Guards recursive dep loads against cycles (mesh→mat→…→mesh).
    std::unordered_set<std::string> _loadingPaths;
    HotReloadWatcher::FileChangeCallback _userHotReloadCb;
    bool _autoWatchLoaded = true;
    // Directory containing the opened ship DB (for relative pak resolution).
    std::string _dbBaseDir;
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
