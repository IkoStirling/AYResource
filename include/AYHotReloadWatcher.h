#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <chrono>
#include <optional>

#include "aytime/TimePoint.h"

namespace ayt::resource
{

// ============================================================
// HotReloadWatcher - 热重载监控
// ============================================================
class HotReloadWatcher {
public:
    using FileChangeCallback = std::function<void(const std::string& path)>;

    HotReloadWatcher();
    ~HotReloadWatcher();

    // ===== Watch management =====
    void watch(const std::string& filepath);
    void unwatch(const std::string& filepath);
    void unwatchAll();

    // ===== Polling =====
    void update();
    void setPollInterval(float seconds);

    // ===== Callbacks =====
    void setOnFileChanged(FileChangeCallback callback);

    // ===== Status =====
    bool isWatching(const std::string& filepath) const;
    size_t watchCount() const;

private:
    struct WatchedFile {
        std::string path;
        // AYTime v1.1 (2026-07-20): value-type TimePoint via std::optional;
        // nullopt means "file did not exist when watch() was called" or
        // "the platform couldn't read mtime". The legacy code stored
        // unique_ptr<ITimePoint> here, which forced a heap alloc per file
        // and made the equality comparison awkward.
        std::optional<ayt::time::TimePoint> lastModified;
        bool existed = false;
    };

    void checkForChanges();

    std::unordered_map<std::string, WatchedFile> watchedFiles;
    std::unordered_set<std::string> pendingReload;
    FileChangeCallback onFileChanged;
    float pollInterval = 1.0f;
    std::chrono::steady_clock::time_point lastPoll;
};

} // namespace ayt::resource