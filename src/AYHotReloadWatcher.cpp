#include "AYHotReloadWatcher.h"
#include "ayio/File.h"

namespace ayt::resource
{

HotReloadWatcher::HotReloadWatcher()
    : lastPoll(std::chrono::steady_clock::now()) {
}

HotReloadWatcher::~HotReloadWatcher() = default;

void HotReloadWatcher::watch(const std::string& filepath) {
    if (watchedFiles.find(filepath) != watchedFiles.end()) {
        return;
    }

    WatchedFile wf;
    wf.path = filepath;
    wf.lastModified = ayt::io::File::lastModifiedTimePoint(filepath);
    wf.existed = wf.lastModified != nullptr;

    watchedFiles.emplace(filepath, std::move(wf));
}

void HotReloadWatcher::unwatch(const std::string& filepath) {
    watchedFiles.erase(filepath);
    pendingReload.erase(filepath);
}

void HotReloadWatcher::unwatchAll() {
    watchedFiles.clear();
    pendingReload.clear();
}

void HotReloadWatcher::update() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(now - lastPoll).count();

    if (elapsed >= pollInterval) {
        checkForChanges();
        lastPoll = now;
    }
}

void HotReloadWatcher::setPollInterval(float seconds) {
    pollInterval = seconds;
}

void HotReloadWatcher::setOnFileChanged(FileChangeCallback callback) {
    onFileChanged = std::move(callback);
}

bool HotReloadWatcher::isWatching(const std::string& filepath) const {
    return watchedFiles.find(filepath) != watchedFiles.end();
}

size_t HotReloadWatcher::watchCount() const {
    return watchedFiles.size();
}

void HotReloadWatcher::checkForChanges() {
    for (auto& [path, wf] : watchedFiles) {
        std::unique_ptr<ayt::time::ITimePoint> currentMod(ayt::io::File::lastModifiedTimePoint(path));
        if (wf.lastModified && currentMod && *wf.lastModified != *currentMod) {
            wf.lastModified = std::move(currentMod);
            pendingReload.insert(path);
            if (onFileChanged) {
                onFileChanged(path);
            }
        }
    }
}

} // namespace ayt::resource
