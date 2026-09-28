#pragma once

// Module-private publication policy. Injected clock/operation callbacks keep
// the retry contract deterministic in tests without replacing filesystem APIs.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

namespace ayt::resource::detail {

inline constexpr auto kPublishRetryBudget = std::chrono::milliseconds(250);
inline constexpr auto kPublishRetryInterval = std::chrono::milliseconds(10);
inline constexpr std::size_t kPublishMaxAttempts = 26;
#if defined(_WIN32)
inline constexpr bool kWindowsPublishRetry = true;
#else
inline constexpr bool kWindowsPublishRetry = false;
#endif

struct PublishFileResult {
    std::error_code error;
    std::size_t attempts = 0;
    bool exhausted = false;
};

inline bool isWindowsPublishContention(const std::error_code& error)
{
    // ERROR_ACCESS_DENIED can also be permanent: never suppress the final error.
    return error.category() == std::system_category()
        && (error.value() == 5 || error.value() == 32 || error.value() == 33);
}

template<class Operation, class Notice, class Clock, class Wait>
PublishFileResult retryPublishFileOperation(Operation operation, Notice notice,
    Clock now, Wait wait, bool windowsRetry = kWindowsPublishRetry)
{
    PublishFileResult result;
    const auto deadline = now() + kPublishRetryBudget;
    for (;;) {
        ++result.attempts;
        result.error.clear();
        operation(result.error);
        if (!result.error || !windowsRetry || !isWindowsPublishContention(result.error))
            return result;
        if (result.attempts >= kPublishMaxAttempts || now() >= deadline) {
            result.exhausted = true;
            return result;
        }
        notice(result.attempts + 1, result.error);
        // A callback or a slow filesystem call can consume the remaining budget.
        auto remaining = deadline - now();
        if (remaining <= decltype(remaining)::zero()) {
            result.exhausted = true;
            return result;
        }
        wait(std::min(std::chrono::duration_cast<decltype(remaining)>(kPublishRetryInterval), remaining));
        if (now() >= deadline) {
            result.exhausted = true;
            return result;
        }
    }
}

template<class Operation, class Notice>
PublishFileResult retryPublishFileOperation(Operation operation, Notice notice)
{
    return retryPublishFileOperation(operation, notice,
        [] { return std::chrono::steady_clock::now(); },
        [](auto duration) { std::this_thread::sleep_for(duration); });
}

inline std::string publishFileError(const std::string& phase,
    const std::filesystem::path& source, const std::filesystem::path& target,
    const PublishFileResult& result)
{
    return "Could not " + phase + ": " + result.error.message()
        + " [source=" + source.generic_string()
        + (target.empty() ? "" : "; target=" + target.generic_string())
        + "; error=" + std::to_string(result.error.value()) + "/" + result.error.category().name()
        + "; attempts=" + std::to_string(result.attempts)
        + (result.exhausted ? "; retry budget exhausted" : "") + "]";
}

} // namespace ayt::resource::detail
