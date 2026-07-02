#include "AYResourceCache.h"
#include "IAYResource.h"
#include <algorithm>

namespace ayt::resource
{

ResourceCache::ResourceCache(const Config& _config)
    : _config(_config) {
}

void ResourceCache::put(const std::string& path, std::shared_ptr<IResource> resource) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_strongCache.find(path) != _strongCache.end()) {
        return;
    }

    while (_config.enableLRU &&
           (_strongCache.size() >= _config.maxResourceCount ||
            _memoryUsage + resource->sizeInBytes() > _config.maxMemoryBytes)) {
        _evictLRU();
    }

    _strongCache[path] = resource;
    _memoryUsage += resource->sizeInBytes();
    _lastUsed[path] = ++_currentTime;
    _weakCache[path] = std::weak_ptr<IResource>(resource);
}

std::shared_ptr<IResource> ResourceCache::get(const std::string& path) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _strongCache.find(path);
    if (it != _strongCache.end()) {
        _lastUsed[path] = ++_currentTime;
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<IResource> ResourceCache::get(const std::string& path) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _strongCache.find(path);
    if (it != _strongCache.end()) {
        return it->second;
    }
    return nullptr;
}

void ResourceCache::remove(const std::string& path) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _strongCache.find(path);
    if (it != _strongCache.end()) {
        _memoryUsage -= it->second->sizeInBytes();
        _strongCache.erase(it);
    }
    _weakCache.erase(path);
    _lastUsed.erase(path);
}

bool ResourceCache::contains(const std::string& path) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _strongCache.find(path) != _strongCache.end();
}

void ResourceCache::putStrong(const std::string& path, std::shared_ptr<IResource> resource) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_strongCache.find(path) != _strongCache.end()) {
        return;
    }

    while (_config.enableLRU &&
           (_strongCache.size() >= _config.maxResourceCount ||
            _memoryUsage + resource->sizeInBytes() > _config.maxMemoryBytes)) {
        _evictLRU();
    }

    _strongCache[path] = resource;
    _memoryUsage += resource->sizeInBytes();
    _lastUsed[path] = ++_currentTime;
    _weakCache[path] = std::weak_ptr<IResource>(resource);
}

std::shared_ptr<IResource> ResourceCache::getStrong(const std::string& path) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _strongCache.find(path);
    if (it != _strongCache.end()) {
        _lastUsed[path] = ++_currentTime;
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<IResource> ResourceCache::tryGetStrong(const std::string& path) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _strongCache.find(path);
    if (it != _strongCache.end()) {
        if (it->second) {
            _lastUsed[path] = ++_currentTime;
            return it->second;
        }
    }
    return nullptr;
}

void ResourceCache::trimToConfig() {
    std::lock_guard<std::mutex> lock(_mutex);
    while (_strongCache.size() > _config.maxResourceCount ||
           _memoryUsage > _config.maxMemoryBytes) {
        if (_strongCache.empty()) break;
        _evictLRU();
    }
}

void ResourceCache::clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    _strongCache.clear();
    _weakCache.clear();
    _lastUsed.clear();
    _memoryUsage = 0;
}

void ResourceCache::_evictLRU() {
    if (_strongCache.empty()) return;

    // Find oldest resource that has no active handles
    uint64_t oldestTime = UINT64_MAX;
    std::string oldestPath;

    for (const auto& [path, time] : _lastUsed) {
        auto handleIt = _handleCount.find(path);
        size_t handles = (handleIt != _handleCount.end()) ? handleIt->second : 0;

        // Skip resources with active handles (protected from eviction)
        if (handles > 0) continue;

        if (time < oldestTime) {
            oldestTime = time;
            oldestPath = path;
        }
    }

    // If all resources have handles, evict the oldest anyway (shouldn't happen often)
    if (oldestPath.empty()) {
        for (const auto& [path, time] : _lastUsed) {
            if (time < oldestTime) {
                oldestTime = time;
                oldestPath = path;
            }
        }
    }

    if (!oldestPath.empty()) {
        auto it = _strongCache.find(oldestPath);
        if (it != _strongCache.end()) {
            _memoryUsage -= it->second->sizeInBytes();
            _strongCache.erase(it);
        }
        _weakCache.erase(oldestPath);
        _lastUsed.erase(oldestPath);
        _handleCount.erase(oldestPath);
    }
}

void ResourceCache::registerHandle(const std::string& path) {
    std::lock_guard<std::mutex> lock(_mutex);
    _handleCount[path]++;
}

void ResourceCache::unregisterHandle(const std::string& path) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _handleCount.find(path);
    if (it != _handleCount.end()) {
        if (it->second > 1) {
            it->second--;
        } else {
            _handleCount.erase(it);
        }
    }
}

size_t ResourceCache::getHandleCount(const std::string& path) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _handleCount.find(path);
    return (it != _handleCount.end()) ? it->second : 0;
}

} // namespace ayt::resource