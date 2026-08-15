#pragma once

namespace ayt::resource
{

// Register all built-in loaders and extension mappings. Idempotent.
bool initializeLoaders();

bool areLoadersInitialized();

} // namespace ayt::resource
