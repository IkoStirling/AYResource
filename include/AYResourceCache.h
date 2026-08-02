#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>

namespace ayt::resource
{

class IResource;

// P3: observable cache budget / hit rates.
struct CacheStats {
    size_t memoryBytes = 0;
    size_t memoryBudget = 0;
    size_t strongCount = 0;
    size_t weakCount = 0;
    size_t graceCount = 0;
    size_t hitCount = 0;
    size_t missCount = 0;
    size_t resurrectCount = 0;
};

// ============================================================
// ResourceCache - LRU + weak resurrection (P3)
// ============================================================
class ResourceCache {
public:
    struct Config {
        size_t maxMemoryBytes = 512 * 1024 * 1024;  // 512 MB
        size_t maxResourceCount = 200;
        bool enableLRU = true;
        // After demote (LRU / last handle), keep a strong grace pin so a
        // quick re-get resurrects without reloading from disk.
        float weakGraceSeconds = 2.0f;
    };

    struct SnapshotEntry {
        std::string path;
        std::string type;
        size_t sizeInBytes = 0;
    };

    ResourceCache() = default;
    explicit ResourceCache(const Config& config);

    void put(const std::string& path, std::shared_ptr<IResource> resource);
    std::shared_ptr<IResource> get(const std::string& path);
    std::shared_ptr<IResource> get(const std::string& path) const;
    void remove(const std::string& path);
    bool contains(const std::string& path) const;

    void putStrong(const std::string& path, std::shared_ptr<IResource> resource);
    std::shared_ptr<IResource> getStrong(const std::string& path);
    std::shared_ptr<IResource> tryGetStrong(const std::string& path);

    void trimToConfig();
    void clear();
    /// F3.4: replace the cache's config + state synchronously under the
    /// internal mutex. The previous setCacheConfig on ResourceManager
    /// used placement-new on the non-trivially-destructible member,
    /// which is UB if any other thread is reading the cache at the
    /// moment the destructor runs. The lock here only serialises calls
    /// to rebuild() — the long-standing getStrongCache() returning a
    /// reference while releasing the lock is a separate concern (see
    /// the existing WARNING comment in this header).
    void rebuild(const Config& config);
    /// Expire grace pins; call from ResourceManager::update.
    void tick(float deltaTime);

    size_t memoryUsage() const;
    size_t strongCount() const;
    size_t weakCount() const;
    size_t graceCount() const;
    CacheStats stats() const;
    const Config& config() const { return _config; }

    void registerHandle(const std::string& path);
    void unregisterHandle(const std::string& path);
    size_t getHandleCount(const std::string& path) const;

    std::vector<SnapshotEntry> snapshotStrong() const;

    // Legacy iteration helper (copies under lock — safe).
    const std::unordered_map<std::string, std::shared_ptr<IResource>>& getStrongCache() const {
        // WARNING: historically returned a reference while releasing the lock.
        // Prefer snapshotStrong(). Kept for existing call sites that only
        // read during single-threaded tests / tag unload.
        return _strongCache;
    }

private:
    struct GraceEntry {
        std::shared_ptr<IResource> resource;
        std::chrono::steady_clock::time_point expireAt{};
    };

    void _evictLRU();
    void _demoteToGraceUnlocked(const std::string& path);
    void _promoteUnlocked(const std::string& path, std::shared_ptr<IResource> resource);
    void _expireGraceUnlocked();
    bool _overBudgetUnlocked(size_t additionalBytes) const;

    Config _config;
    mutable std::mutex _mutex;
    size_t _memoryUsage = 0;
    size_t _hitCount = 0;
    size_t _missCount = 0;
    size_t _resurrectCount = 0;

    std::unordered_map<std::string, std::shared_ptr<IResource>> _strongCache;
    std::unordered_map<std::string, std::weak_ptr<IResource>> _weakCache;
    std::unordered_map<std::string, GraceEntry> _graceCache;
    std::unordered_map<std::string, uint64_t> _lastUsed;
    std::unordered_map<std::string, size_t> _handleCount;
    uint64_t _currentTime = 0;
};

} // namespace ayt::resource
