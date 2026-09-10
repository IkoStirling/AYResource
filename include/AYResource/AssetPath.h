#pragma once

#include <string>
#include <vector>

namespace ayt::resource
{

void setAssetRoot(const std::string& path);
const std::string& assetRoot();
void setAssetRoots(const std::vector<std::string>& paths);
const std::vector<std::string>& assetRoots();

// Resolve refPath relative to basePath (usually the referring asset file path).
// Absolute refPath is returned unchanged.
std::string resolveAssetPath(const std::string& basePath, const std::string& refPath);

} // namespace ayt::resource
