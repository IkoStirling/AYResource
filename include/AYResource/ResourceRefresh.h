#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::resource {

/// One synchronous consumer outcome; independent of any rendering backend.
enum class ResourceRefreshState : uint8_t {
    Updated,
    Retained,
    NotResident,
    NotHandled,
    Unconfirmed,
    Failed,
};

struct ResourceConsumerRefresh {
    ResourceRefreshState state = ResourceRefreshState::NotHandled;
    std::string variant;
    uint64_t resourceId = 0;
    uint64_t contextId = 0;
    std::string diagnostic;
};

struct ResourcePathRefresh {
    std::string path;
    std::string consumer;
    std::vector<ResourceConsumerRefresh> outcomes;
};

/// Describes a completed CPU refresh followed by synchronous consumer calls.
/// Neither this report nor its opaque ids retain GPU resources. A successful
/// CPU refresh does not imply consumer success or atomic GPU batch publication.
struct ResourceRefreshReport {
    bool cpuRefreshed = false;
    std::string error;
    std::vector<ResourcePathRefresh> paths;

    bool needsAttention() const noexcept {
        if (!cpuRefreshed) return true;
        for (const auto& path : paths)
            for (const auto& outcome : path.outcomes)
                if (outcome.state == ResourceRefreshState::Retained
                    || outcome.state == ResourceRefreshState::Failed) return true;
        return false;
    }
};

} // namespace ayt::resource
