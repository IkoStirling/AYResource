#pragma once
#include "ImportNativePath.h"
#include <AYResource/VirtualAssetPath.h>
#include <AYIO/PathSafety.h>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace ayt::resource::detail {
inline std::string sceneTextureSourceKey(const std::string& reference, const std::string& directory)
{
    auto spelling = reference;
    for (auto& ch : spelling) if (ch == '\\') ch = '/';
    const auto root = std::filesystem::absolute(std::filesystem::u8path(directory.empty() ? "." : directory)).lexically_normal();
    auto path = std::filesystem::u8path(spelling);
    if (path.is_relative()) path = root / path;
    path = path.lexically_normal();
    auto relative = path.lexically_relative(root);
    return importPortablePathText(relative.empty() ? path : relative);
}
inline std::string sceneTextureSourceHash(const std::string& key)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : key) { hash ^= byte; hash *= 1099511628211ULL; }
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << hash;
    return text.str();
}
inline std::string safeSceneTextureStem(const std::string& reference, const std::string& directory)
{
    const auto legacy = makeTextureStemFromSourcePath(reference);
    if (ayt::io::path::isPortableRelative(legacy)) return legacy;
    auto spelling = reference;
    for (auto& ch : spelling) if (ch == '\\') ch = '/';
    auto leaf = importPortablePathText(std::filesystem::u8path(spelling).stem());
    if (!ayt::io::path::isPortableRelative(leaf)) leaf = "image";
    return leaf + "_" + sceneTextureSourceHash(sceneTextureSourceKey(reference, directory));
}
}
