#pragma once
#include <string>
#include <memory>
#include <functional>
#include <future>
#include <queue>
#include <list>
#include <mutex>
#include <atomic>

namespace ayt::resource
{

class IResource;

// ============================================================
// AsyncLoader - 异步加载队列
// ============================================================
//
// P1 Progress/Cancel:
// - cancel(path) / cancelAll() 取消加载任务
// - onProgress 回调报告加载进度 (0.0 ~ 1.0)
// - 进度基于加载阶段：Queued → Loading → Completed
//
class AsyncLoader {
public:
    // Progress callback: (path, progress 0.0~1.0)
    using ProgressCallback = std::function<void(const std::string& path, float progress)>;

    AsyncLoader();
    ~AsyncLoader();

    // ===== Async loading with progress =====
    std::shared_future<std::shared_ptr<IResource>> loadAsync(
        const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {},
        ProgressCallback onProgress = {}
    );

    // Legacy overload without progress
    std::shared_future<std::shared_ptr<IResource>> loadAsync(
        const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback
    );

    // ===== Queue management =====
    void update(float deltaTime);
    void cancel(const std::string& path);
    void cancelAll();

    // ===== Status =====
    size_t pendingCount() const;
    bool isLoading(const std::string& path) const;

private:
    struct LoadTask {
        std::string path;
        std::promise<std::shared_ptr<IResource>> promise;
        std::function<void(std::shared_ptr<IResource>)> callback;
        ProgressCallback onProgress;
        bool cancelled = false;
    };

    void processQueue();

    std::list<std::unique_ptr<LoadTask>> queue;
    mutable std::mutex mutex;
    std::atomic<bool> running{false};
    float deltaTime = 0.0f;
};

} // namespace ayt::resource