#pragma once
#include <string>
#include <memory>
#include <functional>
#include <future>
#include <vector>
#include <mutex>
#include <atomic>

namespace ayt::task {
class ITask;
class ITaskScheduler;
}

namespace ayt::resource
{

class IResource;

// ============================================================
// AsyncLoader - 异步加载（P3: AYTask 线程池）
// ============================================================
//
// - loadAsync submits work to ITaskScheduler::defaultScheduler()
// - cancel / cancelAll are cooperative (atomic flag + ITask::cancel)
// - Completion always satisfies the future (resource or nullptr)
//
class AsyncLoader {
public:
    using ProgressCallback = std::function<void(const std::string& path, float progress)>;

    AsyncLoader();
    explicit AsyncLoader(ayt::task::ITaskScheduler& scheduler);
    ~AsyncLoader();

    AsyncLoader(const AsyncLoader&) = delete;
    AsyncLoader& operator=(const AsyncLoader&) = delete;

    std::shared_future<std::shared_ptr<IResource>> loadAsync(
        const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {},
        ProgressCallback onProgress = {}
    );

    std::shared_future<std::shared_ptr<IResource>> loadAsync(
        const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback
    );

    void update(float deltaTime);
    void cancel(const std::string& path);
    void cancelAll();

    size_t pendingCount() const;
    bool isLoading(const std::string& path) const;

private:
    struct LoadTask {
        std::string path;
        std::promise<std::shared_ptr<IResource>> promise;
        std::function<void(std::shared_ptr<IResource>)> callback;
        ProgressCallback onProgress;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> completed{false};
    };

    struct Inflight {
        std::shared_ptr<LoadTask> load;
        ayt::task::ITask* ayTask = nullptr;
    };

    void reapCompletedUnlocked();
    void waitAndClearInflight();

    ayt::task::ITaskScheduler* _scheduler = nullptr;
    std::vector<Inflight> _inflight;
    mutable std::mutex _mutex;
};

} // namespace ayt::resource
