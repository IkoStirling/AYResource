#include "AYResource/AssetPath.h"

#include <AYIO/Path.h>
#include <AYIO/File.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace ayt::resource
{

namespace {

std::vector<std::string>& assetRootsStorage()
{
    static std::vector<std::string> roots;
    return roots;
}

std::string resolveFromRoots(const std::string& refPath)
{
    const auto& roots = assetRootsStorage();
    if (roots.empty()) return {};
    std::string first;
    for (const std::string& root : roots) {
        const std::string candidate = ayt::io::path::normalize(
            ayt::io::path::join(root, refPath));
        if (first.empty()) first = candidate;
        if (ayt::io::File::exists(candidate)) return candidate;
    }
    return first;
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
    setAssetRoots(path.empty() ? std::vector<std::string>{}
                               : std::vector<std::string>{path});
}

const std::string& assetRoot()
{
    static const std::string empty;
    const auto& roots = assetRootsStorage();
    return roots.empty() ? empty : roots.front();
}

void setAssetRoots(const std::vector<std::string>& paths)
{
    auto& roots = assetRootsStorage();
    roots.clear();
    for (const std::string& path : paths) {
        if (path.empty()) continue;
        const std::string normalized = ayt::io::path::normalize(path);
        if (std::find(roots.begin(), roots.end(), normalized) == roots.end()) {
            roots.push_back(normalized);
        }
    }
}

const std::vector<std::string>& assetRoots()
{
    return assetRootsStorage();
}

std::string resolveAssetPath(const std::string& basePath, const std::string& refPath)
{
    if (refPath.empty()) {
        return refPath;
    }
    if (ayt::io::path::isAbsolute(refPath)) {
        return ayt::io::path::normalize(refPath);
    }

    // Contract virtual paths must resolve against the asset root, not the
    // referring file's directory (otherwise meshes/foo.aymesh + materials/x
    // becomes meshes/materials/x).
    if (!assetRoots().empty() && isRootRelativeVirtualPath(refPath)) {
        return resolveFromRoots(refPath);
    }

    std::string baseDir = ayt::io::path::directory(basePath);
    if (!baseDir.empty() && baseDir != ".") {
        return ayt::io::path::normalize(ayt::io::path::join(baseDir, refPath));
    }

    if (!assetRoots().empty()) {
        return resolveFromRoots(refPath);
    }

    return ayt::io::path::normalize(refPath);
}

} // namespace ayt::resource
