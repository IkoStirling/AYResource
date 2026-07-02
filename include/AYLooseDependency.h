#pragma once

#include <string>
#include <vector>

namespace ayt::resource
{

// Sidecar path for a loose asset: {dir}/{stem}.aydep.json
std::string looseDependencySidecarPath(const std::string& assetPath);

// Resolved dependency targets for assetPath (empty if no sidecar / no matches).
std::vector<std::string> collectLooseDependencies(const std::string& assetPath);

} // namespace ayt::resource
