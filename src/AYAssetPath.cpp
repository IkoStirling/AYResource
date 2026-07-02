#include "AYAssetPath.h"

#include <AYPath.h>

namespace ayt::resource
{

namespace {

std::string& assetRootStorage()
{
    static std::string root;
    return root;
}

} // namespace

void setAssetRoot(const std::string& path)
{
    assetRootStorage() = path.empty() ? std::string{} : ayt::io::path::normalize(path);
}

const std::string& assetRoot()
{
    return assetRootStorage();
}

std::string resolveAssetPath(const std::string& basePath, const std::string& refPath)
{
    if (refPath.empty()) {
        return refPath;
    }
    if (ayt::io::path::isAbsolute(refPath)) {
        return ayt::io::path::normalize(refPath);
    }

    std::string baseDir = ayt::io::path::directory(basePath);
    if (!baseDir.empty() && baseDir != ".") {
        return ayt::io::path::normalize(ayt::io::path::join(baseDir, refPath));
    }

    const std::string& root = assetRoot();
    if (!root.empty()) {
        return ayt::io::path::normalize(ayt::io::path::join(root, refPath));
    }

    return ayt::io::path::normalize(refPath);
}

} // namespace ayt::resource
