#include "AYLooseDependency.h"

#include "AYAssetPath.h"
#include "IAYConverter.h"
#include "IAYMesh.h"
#include "IAYMaterial.h"
#include "AYIntermediateAsset.h"
#include "AYMaterial.h"

#include <AYIO/File.h>
#include <AYIO/Path.h>

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

bool isTextureParamType(MaterialParamType type)
{
    return type == MaterialParamType::Texture2D
        || type == MaterialParamType::Texture3D
        || type == MaterialParamType::TextureCube;
}

void pushUniqueResolved(std::vector<std::string>& out,
                        const std::string& assetPath,
                        const char* ref)
{
    if (ref == nullptr || ref[0] == '\0') {
        return;
    }
    const std::string resolved = resolveAssetPath(assetPath, ref);
    if (resolved.empty()) {
        return;
    }
    for (const std::string& existing : out) {
        if (existing == resolved) {
            return;
        }
    }
    out.push_back(resolved);
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

std::vector<std::string> collectIntrinsicDependencies(const std::string& assetPath,
                                                      const IResource& resource)
{
    std::vector<std::string> resolved;

    if (const auto* mesh = dynamic_cast<const IMesh*>(&resource)) {
        const UInt32 slotCount = mesh->getMaterialSlotCount();
        for (UInt32 i = 0; i < slotCount; ++i) {
            pushUniqueResolved(resolved, assetPath, mesh->getMaterialSlot(i));
        }
        return resolved;
    }

    if (const auto* material = dynamic_cast<const IMaterial*>(&resource)) {
        // Concrete Material exposes forEachParameter; IMaterial only has named getters.
        if (const auto* concrete = dynamic_cast<const Material*>(material)) {
            concrete->forEachParameter([&](const char* name, MaterialParamType type) {
                if (!isTextureParamType(type)) {
                    return;
                }
                pushUniqueResolved(resolved, assetPath, concrete->getTexture(name));
            });
        }
        return resolved;
    }

    (void)assetPath;
    return resolved;
}

} // namespace ayt::resource
