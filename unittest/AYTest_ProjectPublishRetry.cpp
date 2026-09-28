#include "AYTest.h"
#include "../src/ProjectPublishRetry.h"

using namespace ayt::resource::detail;
using namespace std::chrono_literals;

TEST_SUITE(ProjectPublishRetryTests)

TEST_CASE(transient_windows_errors_recover_and_clear_the_original_error)
{
    for (int error : {5, 32, 33}) {
        auto clock = std::chrono::steady_clock::time_point{};
        int calls = 0, notices = 0;
        const auto result = retryPublishFileOperation(
            [&](std::error_code& ec) { if (++calls == 1) ec = {error, std::system_category()}; },
            [&](std::size_t attempt, const std::error_code& ec) {
                ++notices; CHECK(attempt == 2u); CHECK(ec.value() == error);
            }, [&] { return clock; }, [&](auto delay) { clock += delay; }, true);
        CHECK(!result.error); CHECK(result.attempts == 2u); CHECK(!result.exhausted);
        CHECK(notices == 1);
    }
}

TEST_CASE(permanent_errors_and_non_windows_operations_are_not_retried)
{
    for (const auto& error : {std::error_code(183, std::system_category()),
                             std::make_error_code(std::errc::permission_denied)}) {
        const auto result = retryPublishFileOperation(
            [&](std::error_code& ec) { ec = error; },
            [](auto, auto) { CHECK_MSG(false, "Non-transient error retried"); },
            [] { return std::chrono::steady_clock::time_point{}; },
            [](auto) { CHECK_MSG(false, "Non-transient error waited"); }, true);
        CHECK(result.error == error); CHECK(result.attempts == 1u); CHECK(!result.exhausted);
    }
    const auto result = retryPublishFileOperation(
        [](std::error_code& ec) { ec = {5, std::system_category()}; },
        [](auto, auto) { CHECK_MSG(false, "Non-Windows operation retried"); },
        [] { return std::chrono::steady_clock::time_point{}; },
        [](auto) { CHECK_MSG(false, "Non-Windows operation waited"); }, false);
    CHECK(result.attempts == 1u); CHECK(!result.exhausted);
}

TEST_CASE(persistent_access_denied_exhausts_the_budget_and_preserves_diagnostics)
{
    auto clock = std::chrono::steady_clock::time_point{};
    const auto result = retryPublishFileOperation(
        [](std::error_code& ec) { ec = {5, std::system_category()}; }, [](auto, auto) {},
        [&] { return clock; }, [&](auto delay) { clock += delay; }, true);
    CHECK(result.error.value() == 5); CHECK(result.exhausted);
    CHECK(result.attempts > 1u); CHECK(result.attempts <= kPublishMaxAttempts);
    CHECK(clock.time_since_epoch() == kPublishRetryBudget);
    const auto text = publishFileError("publish package", "test.staging", "test", result);
    CHECK(text.find("source=test.staging") != std::string::npos);
    CHECK(text.find("target=test") != std::string::npos);
    CHECK(text.find("error=5/system") != std::string::npos);
    CHECK(text.find("attempts=") != std::string::npos);
    CHECK(text.find("retry budget exhausted") != std::string::npos);
}

TEST_CASE(attempt_limit_also_bounds_a_clock_that_does_not_advance)
{
    const auto result = retryPublishFileOperation(
        [](std::error_code& ec) { ec = {32, std::system_category()}; }, [](auto, auto) {},
        [] { return std::chrono::steady_clock::time_point{}; }, [](auto) {}, true);
    CHECK(result.exhausted); CHECK(result.attempts == kPublishMaxAttempts);
}

TEST_CASE(slow_operation_or_callback_does_not_extend_the_deadline)
{
    auto clock = std::chrono::steady_clock::time_point{};
    const auto slowOperation = retryPublishFileOperation(
        [&](std::error_code& ec) { ec = {5, std::system_category()}; clock += 1s; },
        [](auto, auto) { CHECK_MSG(false, "Expired operation retried"); },
        [&] { return clock; }, [](auto) { CHECK_MSG(false, "Expired operation waited"); }, true);
    CHECK(slowOperation.exhausted); CHECK(slowOperation.attempts == 1u);
    clock = {};
    const auto slowCallback = retryPublishFileOperation(
        [](std::error_code& ec) { ec = {5, std::system_category()}; },
        [&](auto, auto) { clock += 1s; }, [&] { return clock; },
        [](auto) { CHECK_MSG(false, "Expired callback waited"); }, true);
    CHECK(slowCallback.exhausted); CHECK(slowCallback.attempts == 1u);
}

TEST_SUITE_END
