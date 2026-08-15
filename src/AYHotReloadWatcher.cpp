#include "AYHotReloadWatcher.h"

#include "AYIO/File.h"
#include "AYIO/FileWatcher.h"
#include "AYIO/Path.h"

#include <vector>

namespace ayt::resource
{

namespace {

std::string normalizeWatchPath(const std::string& path)
{
    return path.empty() ? path : ayt::io::path::normalize(path);
}

} // namespace

HotReloadWatcher::HotReloadWatcher()
    : lastPoll(std::chrono::steady_clock::now())
{
}

HotReloadWatcher::~HotReloadWatcher()
{
    if (_fileWatcher) {
        _fileWatcher->stop();
        _fileWatcher.reset();
    }
}

void HotReloadWatcher::ensureFileWatcherStarted()
{
    if (!_fileWatcher) {
        _fileWatcher = std::make_unique<ayt::io::FileWatcher>();
    }
    if (!_fileWatcher->isRunning()) {
        _fileWatcher->start();
    }
}

void HotReloadWatcher::watch(const std::string& filepath)
{
    const std::string path = normalizeWatchPath(filepath);
    if (path.empty() || watchedFiles.find(path) != watchedFiles.end()) {
        return;
    }

    WatchedFile wf;
    wf.path = path;
    wf.lastModified = ayt::io::File::lastModifiedTimePoint(path);
    wf.existed = wf.lastModified.has_value();
    watchedFiles.emplace(path, std::move(wf));

    ensureFileWatcherStarted();
    if (!_fileWatcher->watch(path, nullptr)) {
        // Directory-or-file watch can fail if the path is missing; mtime
        // fallback still covers the entry once the file appears.
    }
}

void HotReloadWatcher::unwatch(const std::string& filepath)
{
    const std::string path = normalizeWatchPath(filepath);
    watchedFiles.erase(path);
    pendingReload.erase(path);
    debounce.erase(path);
    if (_fileWatcher) {
        (void)_fileWatcher->unwatch(path);
    }
}

void HotReloadWatcher::unwatchAll()
{
    if (_fileWatcher) {
        for (const auto& [path, wf] : watchedFiles) {
            (void)wf;
            (void)_fileWatcher->unwatch(path);
        }
        _fileWatcher->clearPending();
    }
    watchedFiles.clear();
    pendingReload.clear();
    debounce.clear();
}

void HotReloadWatcher::update()
{
    // 1) OS events via FileWatcher (canonical main-thread drain).
    if (_fileWatcher && _fileWatcher->isRunning()) {
        std::vector<ayt::io::FileWatchEvent> events;
        _fileWatcher->pollPending(events);
        for (const ayt::io::FileWatchEvent& ev : events) {
            if (ev.kind == ayt::io::FileWatchEvent::Kind::Deleted) {
                continue;
            }
            const std::string path = normalizeWatchPath(ev.path);
            if (watchedFiles.find(path) == watchedFiles.end()) {
                continue;
            }
            noteChanged(path);
        }
    }

    // 2) mtime fallback (tests / editors that rewrite without OS events).
    const auto now = std::chrono::steady_clock::now();
    const float elapsed =
        std::chrono::duration_cast<std::chrono::duration<float>>(now - lastPoll).count();
    if (elapsed >= pollInterval) {
        checkMtimeFallback();
        lastPoll = now;
    }

    // 3) Fire debounced callbacks.
    flushDebounced();
}

void HotReloadWatcher::setPollInterval(float seconds)
{
    pollInterval = seconds < 0.0f ? 0.0f : seconds;
}

void HotReloadWatcher::setDebounceSeconds(float seconds)
{
    debounceSeconds = seconds < 0.0f ? 0.0f : seconds;
}

void HotReloadWatcher::setOnFileChanged(FileChangeCallback callback)
{
    onFileChanged = std::move(callback);
}

bool HotReloadWatcher::isWatching(const std::string& filepath) const
{
    return watchedFiles.find(normalizeWatchPath(filepath)) != watchedFiles.end();
}

size_t HotReloadWatcher::watchCount() const
{
    return watchedFiles.size();
}

void HotReloadWatcher::checkMtimeFallback()
{
    for (auto& [path, wf] : watchedFiles) {
        std::optional<ayt::time::TimePoint> currentMod =
            ayt::io::File::lastModifiedTimePoint(path);
        if (wf.lastModified && currentMod && *wf.lastModified != *currentMod) {
            wf.lastModified = currentMod;
            noteChanged(path);
        } else if (!wf.lastModified && currentMod) {
            // File appeared after watch() on a missing path.
            wf.lastModified = currentMod;
            wf.existed = true;
            noteChanged(path);
        } else if (currentMod) {
            wf.lastModified = currentMod;
        }
    }
}

void HotReloadWatcher::noteChanged(const std::string& path)
{
    pendingReload.insert(path);
    DebounceEntry& entry = debounce[path];
    // Extend the quiet window while events keep arriving (editor save bursts).
    entry.pending = true;
    entry.since = std::chrono::steady_clock::now();

    // Keep mtime snapshot fresh so fallback doesn't re-fire immediately.
    if (auto it = watchedFiles.find(path); it != watchedFiles.end()) {
        it->second.lastModified = ayt::io::File::lastModifiedTimePoint(path);
        it->second.existed = it->second.lastModified.has_value();
    }
}

void HotReloadWatcher::flushDebounced()
{
    if (!onFileChanged) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto debounceDur = std::chrono::duration<float>(debounceSeconds);

    std::vector<std::string> ready;
    ready.reserve(debounce.size());
    for (auto& [path, entry] : debounce) {
        if (!entry.pending) {
            continue;
        }
        if ((now - entry.since) < debounceDur) {
            continue;
        }
        entry.pending = false;
        ready.push_back(path);
    }

    for (const std::string& path : ready) {
        pendingReload.erase(path);
        onFileChanged(path);
    }
}

} // namespace ayt::resource
