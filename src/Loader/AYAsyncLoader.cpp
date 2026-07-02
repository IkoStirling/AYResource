#include "AYAsyncLoader.h"
#include "AYResourceManager.h"
#include "AYResourceRegistry.h"
#include <algorithm>
#include <thread>
#include <condition_variable>

namespace ayt::resource
{

AsyncLoader::AsyncLoader()
    : running(false)
    , deltaTime(0.0f) {
}

AsyncLoader::~AsyncLoader() {
    cancelAll();
}

std::shared_future<std::shared_ptr<IResource>> AsyncLoader::loadAsync(
    const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback,
    ProgressCallback onProgress) {
    auto task = std::make_unique<LoadTask>();
    task->path = path;
    task->callback = std::move(callback);
    task->onProgress = std::move(onProgress);

    auto future = task->promise.get_future();

    {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(std::move(task));
        running = true;
    }

    // Report progress: Queued
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& t : queue) {
            if (t->path == path && t->onProgress) {
                t->onProgress(path, 0.0f);
                break;
            }
        }
    }

    // 启动后台线程处理加载
    std::thread([this]() {
        std::unique_ptr<LoadTask> task;
        std::string taskPath;

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (queue.empty()) {
                running = false;
                return;
            }

            task = std::move(queue.front());
            queue.pop_front();
            taskPath = task->path;

            // 如果队列空了，停止运行
            running = !queue.empty();
        }

        // Report progress: Loading started (20%)
        if (task->onProgress) {
            task->onProgress(taskPath, 0.2f);
        }

        if (task->cancelled) {
            task->promise.set_value(nullptr);
            if (task->onProgress) {
                task->onProgress(taskPath, 0.0f);  // Cancelled = 0%
            }
            return;
        }

        ResourceManager& rm = ResourceManager::instance();
        auto resource = rm._loadInternal(task->path);

        // Report progress: Loading completed (100%)
        if (task->onProgress) {
            task->onProgress(taskPath, 1.0f);
        }

        task->promise.set_value(resource);

        if (task->callback && resource) {
            task->callback(resource);
        }
    }).detach();

    return std::shared_future<std::shared_ptr<IResource>>(std::move(future));
}

std::shared_future<std::shared_ptr<IResource>> AsyncLoader::loadAsync(
    const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    return loadAsync(path, std::move(callback), nullptr);
}

void AsyncLoader::update(float deltaTime) {
    deltaTime = deltaTime;
    // update 现在只检查状态，不再执行加载
    // 加载已在后台线程异步执行
}

void AsyncLoader::cancel(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& task : queue) {
        if (task->path == path) {
            task->cancelled = true;
            task->promise.set_value(nullptr);
        }
    }
}

void AsyncLoader::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& task : queue) {
        task->cancelled = true;
        task->promise.set_value(nullptr);
    }
    queue.clear();
    running = false;
}

size_t AsyncLoader::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return queue.size();
}

bool AsyncLoader::isLoading(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& task : queue) {
        if (task->path == path) {
            return true;
        }
    }
    return false;
}

} // namespace ayt::resource
