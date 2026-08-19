#pragma once

// AYResource/VirtualAssetPath.h — single naming contract for cooked virtual paths.
//
// FBXParser / MaterialConverter / TextureConverter / FBXConverter(.aydep)
// MUST use these helpers so mesh materialSlots, on-disk .aymat/.aytex, and
// dependency edges all agree. Do not invent parallel path spelling.

#include <cctype>
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

// textures/{stem}{usageSuffix}{ext} — dev raw-reference mode (ImportOptions
// cookTextures=false) keeps the source extension (e.g. ".png") so .aymat
// materials point at the raw image copied into textures/. Release cook
// always uses ".aytex".
inline std::string makeTextureVirtualPath(
    const std::string& stem,
    const std::string& usageSuffix,
    const char* ext) {
    return "textures/" + stem + usageSuffix + ext;
}

// textures/{stem}{usageSuffix}.aytex  — always .aytex (even passthrough cook)
inline std::string makeTextureVirtualPath(
    const std::string& stem,
    const std::string& usageSuffix = kDefaultDiffuseUsageSuffix) {
    return makeTextureVirtualPath(stem, usageSuffix, ".aytex");
}

inline std::string makeTextureVirtualPathFromSource(
    const std::string& sourcePath,
    const std::string& usageSuffix,
    const char* ext) {
    return makeTextureVirtualPath(makeTextureStemFromSourcePath(sourcePath),
                                  usageSuffix, ext);
}

inline std::string makeTextureVirtualPathFromSource(
    const std::string& sourcePath,
    const std::string& usageSuffix = kDefaultDiffuseUsageSuffix) {
    return makeTextureVirtualPathFromSource(sourcePath, usageSuffix, ".aytex");
}

// Extension for a dev raw-reference texture virtual path: the lower-cased
// source extension for authoring formats (".png"/".jpg"/...), ".aytex" for
// dds/aytex sources (their cook path is zero-decode passthrough and .dds
// is not a registered runtime extension). Used by FBXParser (param refs)
// and FBXConverter (.aydep fallback) so the .aymat reference and the
// dependency edge always agree.
inline std::string textureDevExtensionOf(const std::string& sourcePath) {
    const size_t dot = sourcePath.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= sourcePath.size()) {
        return ".aytex";
    }
    std::string e = sourcePath.substr(dot);
    for (char& c : e) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (e == ".dds" || e == ".aytex") {
        return ".aytex";
    }
    return e;
}

// tilemaps/{baseName}.aytilemap — CM-2 (2026-08-11). Single spelling for
// TilemapConverter output and TilemapLoader input; do not invent a
// parallel one in cook tools or tests.
inline std::string makeTilemapVirtualPath(const std::string& baseName) {
    return "tilemaps/" + baseName + ".aytilemap";
}

} // namespace ayt::resource
