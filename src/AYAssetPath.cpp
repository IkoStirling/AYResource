#include "AYAssetPath.h"

#include <ayio/Path.h>
#include <cctype>
#include <cstring>
#include <string>

namespace ayt::resource
{

namespace {

std::string& assetRootStorage()
{
    static std::string root;
    return root;
}

// Cooked virtual paths are always root-relative: materials/, textures/, ...
// Bare filenames (e.g. sibling .phoskia next to an .aymat) stay base-relative.
bool isRootRelativeVirtualPath(const std::string& refPath)
{
    if (refPath.empty()) {
        return false;
    }
    // Absolute / drive-relative / URL-ish — not virtual.
    if (ayt::io::path::isAbsolute(refPath)) {
        return false;
    }
    if (refPath.size() >= 2 && std::isalpha(static_cast<unsigned char>(refPath[0])) &&
        refPath[1] == ':') {
        return false;
    }

    static const char* kPrefixes[] = {
        "materials/", "materials\\",
        "textures/",  "textures\\",
        "meshes/",    "meshes\\",
        "skeletons/", "skeletons\\",
        "animations/","animations\\",
        "shaders/",   "shaders\\",
        "Scripts/",   "Scripts\\",
        "scripts/",   "scripts\\",
    };
    for (const char* prefix : kPrefixes) {
        const size_t n = std::char_traits<char>::length(prefix);
        if (refPath.size() >= n) {
            bool match = true;
            for (size_t i = 0; i < n; ++i) {
                const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(refPath[i])));
                const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
    }
    return false;
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

    const std::string& root = assetRoot();

    // Contract virtual paths must resolve against the asset root, not the
    // referring file's directory (otherwise meshes/foo.aymesh + materials/x
    // becomes meshes/materials/x).
    if (!root.empty() && isRootRelativeVirtualPath(refPath)) {
        return ayt::io::path::normalize(ayt::io::path::join(root, refPath));
    }

    std::string baseDir = ayt::io::path::directory(basePath);
    if (!baseDir.empty() && baseDir != ".") {
        return ayt::io::path::normalize(ayt::io::path::join(baseDir, refPath));
    }

    if (!root.empty()) {
        return ayt::io::path::normalize(ayt::io::path::join(root, refPath));
    }

    return ayt::io::path::normalize(refPath);
}

} // namespace ayt::resource
