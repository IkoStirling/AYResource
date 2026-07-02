#pragma once
#include <memory>
#include <string>
#include <functional>

namespace ayt::resource
{

class IResource;
class ResourceCache;

// ============================================================
// ResourceHandle - 延迟加载句柄（带引用计数）
// ============================================================
//
// P1 Reference Counting:
// - ResourceHandle 在创建时向 ResourceCache 注册
// - ResourceCache 跟踪每个资源路径的 handle 数量
// - 有 active handles 的资源受 LRU 保护，不会被 eviction
// - 当所有 handles 被销毁后，资源变为可驱逐状态
//
// 使用方式:
//   auto handle = rm.createHandle<IMesh>("player.aymesh");
//   auto mesh = handle->get();  // 获取资源（如果未加载则加载）
//   // handle 销毁时，自动减少引用计数
//
template<typename T>
class ResourceHandle : public std::enable_shared_from_this<ResourceHandle<T>> {
public:
    using Ptr = std::shared_ptr<ResourceHandle<T>>;
    using WPtr = std::weak_ptr<ResourceHandle<T>>;
    using LoadCallback = std::function<std::shared_ptr<IResource>(const std::string& path)>;

    ResourceHandle(const std::string& _path, ResourceCache* cache, LoadCallback loader, bool lazyLoad = true);
    ~ResourceHandle();

    // Force load if not loaded
    std::shared_ptr<T> get();

    // Check if loaded
    bool isLoaded() const {
        return resource != nullptr && resource->isLoaded();
    }

    // Get path
    const std::string& getPath() const { return path; }

    // Set load callback (for deferred callback setting)
    void setOnLoaded(std::function<void(std::shared_ptr<T>)> callback) {
        onLoaded = callback;
    }

    // Conversion to shared_ptr<T>
    std::shared_ptr<T> operator->() { return get(); }
    std::shared_ptr<T> operator*() { return get(); }

private:
    void loadNow();

    std::string path;
    ResourceCache* cache = nullptr;  // Pointer to manager's cache (not owned)
    LoadCallback loaderCallback;     // Callback to load resource if not in cache
    bool lazyLoad = true;
    std::shared_ptr<IResource> resource;
    std::function<void(std::shared_ptr<T>)> onLoaded;
};

// Forward declaration
class ResourceManager;

// ============================================================
// ResourceHandle - Template Implementation
// ============================================================

template<typename T>
ResourceHandle<T>::ResourceHandle(const std::string& _path, ResourceCache* _cache, LoadCallback _loader, bool _lazyLoad)
    : path(_path)
    , cache(_cache)
    , loaderCallback(std::move(_loader))
    , lazyLoad(_lazyLoad)
{
    if (cache) {
        cache->registerHandle(path);
    }
}

template<typename T>
ResourceHandle<T>::~ResourceHandle() {
    if (cache) {
        cache->unregisterHandle(path);
    }
}

template<typename T>
std::shared_ptr<T> ResourceHandle<T>::get() {
    if (!resource) {
        loadNow();
    }
    return std::static_pointer_cast<T>(resource);
}

template<typename T>
void ResourceHandle<T>::loadNow() {
    if (resource) return;

    // Try to get from cache first
    if (cache) {
        resource = cache->get(path);
        if (resource) {
            // Notify callback if set
            if (onLoaded) {
                onLoaded(std::static_pointer_cast<T>(resource));
            }
            return;
        }
    }

    // Not in cache, use loader callback to load
    if (loaderCallback) {
        resource = loaderCallback(path);
        if (resource) {
            // Add to strong cache if cache is available
            if (cache) {
                cache->putStrong(path, resource);
            }
            // Notify callback if set
            if (onLoaded) {
                onLoaded(std::static_pointer_cast<T>(resource));
            }
        }
    }
}

} // namespace ayt::resource