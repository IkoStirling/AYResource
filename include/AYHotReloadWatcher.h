#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <chrono>

namespace ayt::time { class ITimePoint; }

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
        std::unique_ptr<ayt::time::ITimePoint> lastModified;
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
