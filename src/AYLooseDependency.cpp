#include "AYLooseDependency.h"

#include "AYAssetPath.h"
#include "IAYConverter.h"

#include <AYFile.h>
#include <AYPath.h>

namespace ayt::resource
{

namespace {

bool pathMatchesAsset(const std::string& assetPath, const std::string& refPath)
{
    const std::string normAsset = ayt::io::path::normalize(assetPath);
    const std::string normRef = ayt::io::path::normalize(refPath);
    if (normAsset == normRef) {
        return true;
    }
    if (normAsset.size() >= normRef.size()) {
        return normAsset.compare(normAsset.size() - normRef.size(), normRef.size(), normRef) == 0;
    }
    return false;
}

} // namespace

std::string looseDependencySidecarPath(const std::string& assetPath)
{
    const std::string dir = ayt::io::path::directory(assetPath);
    const std::string stem = ayt::io::path::stem(assetPath);
    if (!dir.empty() && dir != ".") {
        return ayt::io::path::join(dir, stem + ".aydep.json");
    }
    return stem + ".aydep.json";
}

std::vector<std::string> collectLooseDependencies(const std::string& assetPath)
{
    std::vector<std::string> resolved;
    const std::string sidecarPath = looseDependencySidecarPath(assetPath);
    if (!ayt::io::File::exists(sidecarPath)) {
        return resolved;
    }

    const std::string json = ayt::io::File::readAllText(sidecarPath);
    if (json.empty()) {
        return resolved;
    }
    const ConversionResult depInfo = ConversionResult::fromJson(json);
    for (const auto& dep : depInfo.dependencies) {
        if (!pathMatchesAsset(assetPath, dep.from)) {
            continue;
        }
        resolved.push_back(resolveAssetPath(assetPath, dep.to));
    }
    return resolved;
}

} // namespace ayt::resource
