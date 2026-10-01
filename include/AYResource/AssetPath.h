#pragma once

#include <string>
#include <vector>

namespace ayt::resource
{

void setAssetRoot(const std::string& path);
const std::string& assetRoot();
void setAssetRoots(const std::vector<std::string>& paths);
const std::vector<std::string>& assetRoots();

/// Resolve owner-relative or virtual-root asset references, including effects/.
/// A registered virtual path stays logical for DB/Pak lookup, even with loose
/// roots mounted; otherwise virtual paths resolve below mounted roots.
/// Absolute refPath is normalized and returned unchanged.
std::string resolveAssetPath(const std::string& basePath, const std::string& refPath);

} // namespace ayt::resource
