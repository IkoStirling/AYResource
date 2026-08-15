#pragma once

// AYResource/VirtualAssetPath.h — single naming contract for cooked virtual paths.
//
// FBXParser / MaterialConverter / TextureConverter / FBXConverter(.aydep)
// MUST use these helpers so mesh materialSlots, on-disk .aymat/.aytex, and
// dependency edges all agree. Do not invent parallel path spelling.

#include <cstddef>
#include <string>

namespace ayt::resource {

// Default diffuse usage suffix used by TextureConverter when converting
// external textures referenced by materials (matches TextureConverter::_d).
inline constexpr const char* kDefaultDiffuseUsageSuffix = "_d";

// materials/{baseName}_material_{index}.aymat
inline std::string makeMaterialVirtualPath(const std::string& baseName,
                                           std::size_t index) {
    return "materials/" + baseName + "_material_" + std::to_string(index)
        + ".aymat";
}

// Strip drive / directory / extension and flatten one trailing folder
// separator into '_'. Examples:
//   "tex/skin.png"              -> "tex_skin"
//   "D:\\Models\\tex\\skin.png" -> "skin"   (absolute: keep basename only)
//   "skin.png"                  -> "skin"
inline std::string makeTextureStemFromSourcePath(const std::string& sourcePath) {
    std::string path = sourcePath;
    if (path.size() >= 2 && path[1] == ':') {
        const std::size_t lastSlash = path.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            path = path.substr(lastSlash + 1);
        }
    }

    std::string stem = path;
    const std::size_t dotPos = stem.find_last_of('.');
    if (dotPos != std::string::npos) {
        stem = stem.substr(0, dotPos);
    }
    const std::size_t lastSep = stem.find_last_of("/\\");
    if (lastSep != std::string::npos) {
        stem = stem.substr(0, lastSep) + "_" + stem.substr(lastSep + 1);
    }
    return stem;
}

// textures/{stem}{usageSuffix}.aytex  — always .aytex (even passthrough cook)
inline std::string makeTextureVirtualPath(
    const std::string& stem,
    const std::string& usageSuffix = kDefaultDiffuseUsageSuffix) {
    return "textures/" + stem + usageSuffix + ".aytex";
}

inline std::string makeTextureVirtualPathFromSource(
    const std::string& sourcePath,
    const std::string& usageSuffix = kDefaultDiffuseUsageSuffix) {
    return makeTextureVirtualPath(makeTextureStemFromSourcePath(sourcePath),
                                  usageSuffix);
}

// tilemaps/{baseName}.aytilemap — CM-2 (2026-08-11). Single spelling for
// TilemapConverter output and TilemapLoader input; do not invent a
// parallel one in cook tools or tests.
inline std::string makeTilemapVirtualPath(const std::string& baseName) {
    return "tilemaps/" + baseName + ".aytilemap";
}

} // namespace ayt::resource
