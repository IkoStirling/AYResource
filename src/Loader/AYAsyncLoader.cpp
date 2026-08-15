#include "AYAsyncLoader.h"
#include "AYResourceManager.h"

#include "aytask/ITaskScheduler.h"
#include "aytask/LambdaTask.h"

#include <ayevent/EventBus.h>
#include <ayevent/Events/ResourceEvents.h>

#include <functional>
#include <utility>

namespace ayt::resource
{

namespace {

uint64_t pathHandle(const std::string& path)
{
    return static_cast<uint64_t>(std::hash<std::string>{}(path));
}

void postLoadOutcome(const std::string& path,
                     const std::shared_ptr<IResource>& resource,
                     bool cancelled)
{
    auto& bus = ayt::event::EventBus::instance();
    const uint64_t handle = pathHandle(path);
    if (resource) {
        bus.post(ayt::event::ResourceLoadCompleteEvent{
            handle,
            /*refCount=*/1,
            /*ok=*/true});
        return;
    }
    // Cancel or load failure — Failed + Complete(ok=false) so dependents
    // that only listen to Complete still see a terminal signal.
    bus.post(ayt::event::ResourceLoadFailedEvent{
        handle,
        cancelled ? 1 : 2});
    bus.post(ayt::event::ResourceLoadCompleteEvent{
        handle,
        /*refCount=*/0,
        /*ok=*/false});
}

} // namespace

AsyncLoader::AsyncLoader()
    : AsyncLoader(ayt::task::ITaskScheduler::defaultScheduler())
{
}

AsyncLoader::AsyncLoader(ayt::task::ITaskScheduler& scheduler)
    : _scheduler(&scheduler)
{
}

AsyncLoader::~AsyncLoader()
{
    cancelAll();
    waitAndClearInflight();
}

std::shared_future<std::shared_ptr<IResource>> AsyncLoader::loadAsync(
    const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback,
    ProgressCallback onProgress)
{
    auto load = std::make_shared<LoadTask>();
    load->path = path;
    load->callback = std::move(callback);
    load->onProgress = std::move(onProgress);

    auto future = std::shared_future<std::shared_ptr<IResource>>(load->promise.get_future());

    if (load->onProgress) {
        load->onProgress(path, 0.0f); // Queued
    }

    // NOTE: Do not call ITask::cancel() — LambdaTask skips execute() body when
    // cancelled, which would leave the promise unsatisfied. Cancellation is
    // cooperative via LoadTask::cancelled only.
    ayt::task::ITask* ayTask = ayt::task::makeTask([load]() {
        auto complete = [&](std::shared_ptr<IResource> resource, float progress) {
            const bool cancelled = load->cancelled.load(std::memory_order_acquire);
            try {
                load->promise.set_value(resource);
            } catch (const std::future_error&) {
            }
            load->completed.store(true, std::memory_order_release);
            if (load->onProgress) {
                load->onProgress(load->path, progress);
            }
            postLoadOutcome(load->path, resource, cancelled);
            if (load->callback) {
                load->callback(std::move(resource));
            }
        };

        if (load->onProgress) {
            load->onProgress(load->path, 0.2f); // Loading
        }

        if (load->cancelled.load(std::memory_order_acquire)) {
            complete(nullptr, 0.0f);
            return;
        }

        auto resource = ResourceManager::instance()._loadInternal(load->path);

        if (load->cancelled.load(std::memory_order_acquire)) {
            complete(nullptr, 0.0f);
            return;
        }

        complete(std::move(resource), 1.0f);
    }, "AYResource.AsyncLoad");

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _inflight.push_back(Inflight{load, ayTask});
    }

    _scheduler->submit(ayTask);
    return future;
}

std::shared_future<std::shared_ptr<IResource>> AsyncLoader::loadAsync(
    const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback)
{
    return loadAsync(path, std::move(callback), nullptr);
}

void AsyncLoader::update(float /*deltaTime*/)
{
    std::lock_guard<std::mutex> lock(_mutex);
    reapCompletedUnlocked();
}

void AsyncLoader::cancel(const std::string& path)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& entry : _inflight) {
        if (entry.load && entry.load->path == path) {
            entry.load->cancelled.store(true, std::memory_order_release);
        }
    }
}

void AsyncLoader::cancelAll()
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& entry : _inflight) {
        if (entry.load) {
            entry.load->cancelled.store(true, std::memory_order_release);
        }
    }
}

size_t AsyncLoader::pendingCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    size_t n = 0;
    for (const auto& entry : _inflight) {
        if (entry.load && !entry.load->completed.load(std::memory_order_acquire)) {
            ++n;
        }
    }
    return n;
}

bool AsyncLoader::isLoading(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& entry : _inflight) {
        if (entry.load && entry.load->path == path
            && !entry.load->completed.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void AsyncLoader::reapCompletedUnlocked()
{
    for (auto it = _inflight.begin(); it != _inflight.end();) {
        if (it->ayTask && it->ayTask->isComplete()) {
            delete it->ayTask;
            it->ayTask = nullptr;
            it = _inflight.erase(it);
        } else {
            ++it;
        }
    }
}

void AsyncLoader::waitAndClearInflight()
{
    std::vector<ayt::task::ITask*> toWait;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        toWait.reserve(_inflight.size());
        for (auto& entry : _inflight) {
            if (entry.ayTask) {
                toWait.push_back(entry.ayTask);
                entry.ayTask = nullptr;
            }
        }
        _inflight.clear();
    }

    for (ayt::task::ITask* t : toWait) {
        if (t) {
            _scheduler->wait(t);
            // If the task never ran (cancelled before execute), ensure
            // we don't leak an unsatisfied promise — execute() usually
            // still runs and checks isCancelled. Defensive: if somehow
            // not completed, still delete.
            delete t;
        }
    }
}

} // namespace ayt::resource
