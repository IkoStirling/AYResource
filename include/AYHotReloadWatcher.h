#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <chrono>
#include <optional>
#include <memory>
#include <cstdint>

#include "aytime/TimePoint.h"

namespace ayt::io {
class FileWatcher;
}

namespace ayt::resource
{

// ============================================================
// HotReloadWatcher - 热重载监控 (P2)
//
// Primary: AYIO FileWatcher (OS events, pollPending on update).
// Fallback: mtime poll for platforms/tests where OS events lag.
// Debounce (~100ms) coalesces bursty save/write sequences.
// ============================================================
class HotReloadWatcher {
public:
    using FileChangeCallback = std::function<void(const std::string& path)>;

    HotReloadWatcher();
    ~HotReloadWatcher();

    HotReloadWatcher(const HotReloadWatcher&) = delete;
    HotReloadWatcher& operator=(const HotReloadWatcher&) = delete;

    // ===== Watch management =====
    void watch(const std::string& filepath);
    void unwatch(const std::string& filepath);
    void unwatchAll();

    // ===== Polling =====
    // Drain FileWatcher + mtime fallback; fire debounced callbacks.
    void update();
    void setPollInterval(float seconds);
    void setDebounceSeconds(float seconds);

    // ===== Callbacks =====
    void setOnFileChanged(FileChangeCallback callback);

    // ===== Status =====
    bool isWatching(const std::string& filepath) const;
    size_t watchCount() const;

private:
    struct WatchedFile {
        std::string path;
        std::optional<ayt::time::TimePoint> lastModified;
        bool existed = false;
    };

    struct DebounceEntry {
        bool pending = false;
        std::chrono::steady_clock::time_point since{};
    };

    void ensureFileWatcherStarted();
    void checkMtimeFallback();
    void noteChanged(const std::string& path);
    void flushDebounced();

    std::unordered_map<std::string, WatchedFile> watchedFiles;
    std::unordered_map<std::string, DebounceEntry> debounce;
    std::unordered_set<std::string> pendingReload;
    FileChangeCallback onFileChanged;
    float pollInterval = 0.25f;
    float debounceSeconds = 0.1f;
    std::chrono::steady_clock::time_point lastPoll;

    std::unique_ptr<ayt::io::FileWatcher> _fileWatcher;
};

} // namespace ayt::resource
