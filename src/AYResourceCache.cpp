#include "AYResourceCache.h"
#include "IAYResource.h"

#include <algorithm>
#include <limits>

namespace ayt::resource
{

ResourceCache::ResourceCache(const Config& config)
    : _config(config)
{
}

bool ResourceCache::_overBudgetUnlocked(size_t additionalBytes) const
{
    return _strongCache.size() >= _config.maxResourceCount
        || (_memoryUsage + additionalBytes) > _config.maxMemoryBytes;
}

void ResourceCache::_promoteUnlocked(const std::string& path,
                                     std::shared_ptr<IResource> resource)
{
    if (!resource) {
        return;
    }
    if (_strongCache.find(path) != _strongCache.end()) {
        _lastUsed[path] = ++_currentTime;
        return;
    }

    while (_config.enableLRU && _overBudgetUnlocked(resource->sizeInBytes())) {
        if (_strongCache.empty()) {
            break;
        }
        _evictLRU();
    }

    _strongCache[path] = resource;
    _memoryUsage += resource->sizeInBytes();
    _lastUsed[path] = ++_currentTime;
    _weakCache[path] = std::weak_ptr<IResource>(resource);
    _graceCache.erase(path);
}

void ResourceCache::_demoteToGraceUnlocked(const std::string& path)
{
    auto it = _strongCache.find(path);
    if (it == _strongCache.end()) {
        return;
    }

    auto resource = it->second;
    _memoryUsage -= resource->sizeInBytes();
    _strongCache.erase(it);
    _lastUsed.erase(path);

    // Keep weak for true resurrection if an external shared_ptr still lives.
    _weakCache[path] = std::weak_ptr<IResource>(resource);

    const float grace = _config.weakGraceSeconds;
    if (grace > 0.0f && resource) {
        GraceEntry entry;
        entry.resource = std::move(resource);
        entry.expireAt = std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<float>(grace));
        _graceCache[path] = std::move(entry);
    }
}

void ResourceCache::_expireGraceUnlocked()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = _graceCache.begin(); it != _graceCache.end();) {
        if (it->second.expireAt <= now) {
            const std::string path = it->first;
            it = _graceCache.erase(it);
            // Drop weak if nobody else holds the resource.
            auto wit = _weakCache.find(path);
            if (wit != _weakCache.end() && wit->second.expired()) {
                _weakCache.erase(wit);
            }
        } else {
            ++it;
        }
    }
}

void ResourceCache::put(const std::string& path, std::shared_ptr<IResource> resource)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_strongCache.find(path) != _strongCache.end()) {
        return;
    }
    _promoteUnlocked(path, std::move(resource));
}

std::shared_ptr<IResource> ResourceCache::get(const std::string& path)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (auto it = _strongCache.find(path); it != _strongCache.end()) {
        _lastUsed[path] = ++_currentTime;
        ++_hitCount;
        return it->second;
    }

    if (auto git = _graceCache.find(path); git != _graceCache.end()) {
        auto resource = git->second.resource;
        _graceCache.erase(git);
        _promoteUnlocked(path, resource);
        ++_resurrectCount;
        ++_hitCount;
        return resource;
    }

    if (auto wit = _weakCache.find(path); wit != _weakCache.end()) {
        if (auto resource = wit->second.lock()) {
            _promoteUnlocked(path, resource);
            ++_resurrectCount;
            ++_hitCount;
            return resource;
        }
        _weakCache.erase(wit);
    }

    ++_missCount;
    return nullptr;
}

std::shared_ptr<IResource> ResourceCache::get(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _strongCache.find(path);
    if (it != _strongCache.end()) {
        return it->second;
    }
    auto git = _graceCache.find(path);
    if (git != _graceCache.end()) {
        return git->second.resource;
    }
    auto wit = _weakCache.find(path);
    if (wit != _weakCache.end()) {
        return wit->second.lock();
    }
    return nullptr;
}

void ResourceCache::remove(const std::string& path)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _strongCache.find(path);
    if (it != _strongCache.end()) {
        _memoryUsage -= it->second->sizeInBytes();
        _strongCache.erase(it);
    }
    _graceCache.erase(path);
    _weakCache.erase(path);
    _lastUsed.erase(path);
}

bool ResourceCache::contains(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_strongCache.find(path) != _strongCache.end()) {
        return true;
    }
    if (_graceCache.find(path) != _graceCache.end()) {
        return true;
    }
    auto wit = _weakCache.find(path);
    return wit != _weakCache.end() && !wit->second.expired();
}

void ResourceCache::putStrong(const std::string& path, std::shared_ptr<IResource> resource)
{
    put(path, std::move(resource));
}

std::shared_ptr<IResource> ResourceCache::getStrong(const std::string& path)
{
    return get(path);
}

std::shared_ptr<IResource> ResourceCache::tryGetStrong(const std::string& path)
{
    return get(path);
}

void ResourceCache::trimToConfig()
{
    std::lock_guard<std::mutex> lock(_mutex);
    while (_overBudgetUnlocked(0) && !_strongCache.empty()) {
        _evictLRU();
    }
}

void ResourceCache::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _strongCache.clear();
    _weakCache.clear();
    _graceCache.clear();
    _lastUsed.clear();
    _handleCount.clear();
    _memoryUsage = 0;
}

void ResourceCache::tick(float /*deltaTime*/)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _expireGraceUnlocked();
}

void ResourceCache::_evictLRU()
{
    if (_strongCache.empty()) {
        return;
    }

    uint64_t oldestTime = std::numeric_limits<uint64_t>::max();
    std::string oldestPath;

    for (const auto& [path, time] : _lastUsed) {
        auto handleIt = _handleCount.find(path);
        const size_t handles = (handleIt != _handleCount.end()) ? handleIt->second : 0;
        if (handles > 0) {
            continue;
        }
        if (time < oldestTime) {
            oldestTime = time;
            oldestPath = path;
        }
    }

    if (oldestPath.empty()) {
        for (const auto& [path, time] : _lastUsed) {
            if (time < oldestTime) {
                oldestTime = time;
                oldestPath = path;
            }
        }
    }

    if (!oldestPath.empty()) {
        // P3: demote to grace/weak instead of hard-erasing the weak entry.
        _demoteToGraceUnlocked(oldestPath);
    }
}

void ResourceCache::registerHandle(const std::string& path)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _handleCount[path]++;
}

void ResourceCache::unregisterHandle(const std::string& path)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _handleCount.find(path);
    if (it == _handleCount.end()) {
        return;
    }
    if (it->second > 1) {
        --it->second;
        return;
    }
    _handleCount.erase(it);
    // Last handle dropped → short grace window for resurrection.
    if (_strongCache.find(path) != _strongCache.end()) {
        _demoteToGraceUnlocked(path);
    }
}

size_t ResourceCache::getHandleCount(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _handleCount.find(path);
    return (it != _handleCount.end()) ? it->second : 0;
}

size_t ResourceCache::memoryUsage() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _memoryUsage;
}

size_t ResourceCache::strongCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _strongCache.size();
}

size_t ResourceCache::weakCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _weakCache.size();
}

size_t ResourceCache::graceCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _graceCache.size();
}

CacheStats ResourceCache::stats() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    CacheStats s;
    s.memoryBytes = _memoryUsage;
    s.memoryBudget = _config.maxMemoryBytes;
    s.strongCount = _strongCache.size();
    s.weakCount = _weakCache.size();
    s.graceCount = _graceCache.size();
    s.hitCount = _hitCount;
    s.missCount = _missCount;
    s.resurrectCount = _resurrectCount;
    return s;
}

std::vector<ResourceCache::SnapshotEntry> ResourceCache::snapshotStrong() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<SnapshotEntry> out;
    out.reserve(_strongCache.size());
    for (const auto& [path, resource] : _strongCache) {
        SnapshotEntry e;
        e.path = path;
        e.type = resource ? resource->getType() : std::string{};
        e.sizeInBytes = resource ? resource->sizeInBytes() : 0;
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace ayt::resource
