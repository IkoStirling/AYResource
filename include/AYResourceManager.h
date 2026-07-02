#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <thread>
#include "IAYResource.h"
#include "AYResourceHandle.h"
#include "AYResourceCache.h"
#include "AYResourceRegistry.h"
#include "IAYStorageDatabase.h"
#include "IAYPackageReader.h"
#include "AYAsyncLoader.h"
#include "AYHotReloadWatcher.h"

namespace ayt::resource
{

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

    // ===== Hot reload =====
    void watchResource(const std::string& filepath);
    void unwatchResource(const std::string& filepath);
    void setOnHotReload(HotReloadWatcher::FileChangeCallback callback);

    // ===== Update loop =====
    void update(float deltaTime);

    // ===== Stats =====
    size_t getMemoryUsage() const;
    size_t getResourceCount() const;
    bool isLoaded(const std::string& filepath) const;
    std::shared_ptr<IResource> getResource(const std::string& filepath);

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
    void _onResourceLoaded(const std::string& filepath, std::shared_ptr<IResource> resource);
    std::shared_ptr<ayt::storage::IPackageReader> _getOrOpenPak(const std::string& pakPath);
    std::shared_ptr<IResource> _loadFromDatabase(const std::string& filepath);

    ResourceCache _cache;
    std::unique_ptr<ayt::storage::IStorageDatabase> _db;
    std::unordered_map<std::string, std::shared_ptr<ayt::storage::IPackageReader>> _openedPaks;
    AsyncLoader _asyncLoader;
    HotReloadWatcher _hotReloadWatcher;
    std::unordered_map<std::string, std::string> _resourceTypes;
};

template<typename T>
std::shared_ptr<ResourceHandle<T>> ResourceManager::createHandle(const std::string& filepath) {
    // Create a loader callback that uses ResourceRegistry
    auto loaderCb = [this](const std::string& path) -> std::shared_ptr<IResource> {
        // Try to get type from database or registry
        auto typeIt = _resourceTypes.find(path);
        if (typeIt == _resourceTypes.end()) {
            // Try to infer from path extension
            std::string ext;
            size_t dotPos = path.find_last_of('.');
            if (dotPos != std::string::npos) {
                ext = path.substr(dotPos);
            }
            // Use registry to infer type from extension
            const std::string& type = ayt::resource::ResourceRegistry::getTypeFromExtension(ext);
            if (type.empty()) return nullptr;
            auto loader = ayt::resource::ResourceRegistry::createLoader(type);
            if (!loader) return nullptr;
            return loader->load(path);
        }
        auto loader = ayt::resource::ResourceRegistry::createLoader(typeIt->second);
        if (!loader) return nullptr;
        return loader->load(path);
    };

    return std::make_shared<ResourceHandle<T>>(filepath, &_cache, std::move(loaderCb), true);
}

} // namespace ayt::resource
