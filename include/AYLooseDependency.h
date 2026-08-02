#pragma once

#include <string>
#include <vector>

namespace ayt::resource
{

class IResource;

// Sidecar path for a loose asset: {dir}/{stem}.aydep.json
std::string looseDependencySidecarPath(const std::string& assetPath);

// Resolved dependency targets for assetPath (empty if no sidecar / no matches).
std::vector<std::string> collectLooseDependencies(const std::string& assetPath);

// P1: dependencies implied by the loaded L2 object itself (no sidecar required).
// - IMesh material slots → .aymat paths
// - IMaterial Texture* parameters → .aytex / image paths
// Paths are resolved relative to assetPath via resolveAssetPath.
std::vector<std::string> collectIntrinsicDependencies(const std::string& assetPath,
                                                      const IResource& resource);

} // namespace ayt::resource
