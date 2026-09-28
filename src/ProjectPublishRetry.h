#pragma once
// Compatibility shim for Resource's internal policy tests and cook cache.
#include <AYIO/detail/PublicationRetry.h>
namespace ayt::resource::detail {
using ayt::io::detail::kPublishRetryBudget;
using ayt::io::detail::kPublishRetryInterval;
using ayt::io::detail::kPublishMaxAttempts;
using ayt::io::detail::kWindowsPublishRetry;
using ayt::io::detail::PublishFileResult;
using ayt::io::detail::retryPublishFileOperation;
using ayt::io::detail::publishFileError;
}
