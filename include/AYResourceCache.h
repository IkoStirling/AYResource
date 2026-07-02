#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>
#include <mutex>

namespace ayt::resource
{

// ============================================================
// ResourceCache - LRU缓存实现（线程安全）
// ============================================================
class ResourceCache {
public:
    struct Config {
        size_t maxMemoryBytes = 512 * 1024 * 1024;  // 512 MB
        size_t maxResourceCount = 200;
        bool enableLRU = true;
    };

    ResourceCache() = default;
    explicit ResourceCache(const Config& config);

    // ===== Cache operations =====
    void put(const std::string& path, std::shared_ptr<class IResource> resource);
    std::shared_ptr<class IResource> get(const std::string& path);
    std::shared_ptr<class IResource> get(const std::string& path) const;
    void remove(const std::string& path);
    bool contains(const std::string& path) const;

    // ===== Strong/Weak cache =====
    void putStrong(const std::string& path, std::shared_ptr<IResource> resource);
    std::shared_ptr<IResource> getStrong(const std::string& path);
    std::shared_ptr<IResource> tryGetStrong(const std::string& path);

    // ===== LRU eviction =====
    void trimToConfig();
    void clear();

    // ===== Stats =====
    size_t memoryUsage() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _memoryUsage;
    }
    size_t strongCount() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _strongCache.size();
    }
    size_t weakCount() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _weakCache.size();
    }
    const Config& config() const { return _config; }

    // ===== Handle reference counting =====
    // Register/unregister ResourceHandle references
    // When handle count reaches 0, resource becomes eligible for LRU eviction
    void registerHandle(const std::string& path);
    void unregisterHandle(const std::string& path);
    size_t getHandleCount(const std::string& path) const;

    // Expose caches for iteration
    const std::unordered_map<std::string, std::shared_ptr<class IResource>>& getStrongCache() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _strongCache;
    }

private:
    void _evictLRU();

    Config _config;
    mutable std::mutex _mutex;
    size_t _memoryUsage = 0;

    std::unordered_map<std::string, std::shared_ptr<class IResource>> _strongCache;
    std::unordered_map<std::string, std::weak_ptr<class IResource>> _weakCache;
    std::unordered_map<std::string, uint64_t> _lastUsed;
    std::unordered_map<std::string, size_t> _handleCount;  // Track ResourceHandle references
    uint64_t _currentTime = 0;
};

} // namespace ayt::resource