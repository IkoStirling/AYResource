#pragma once
#include "ImportNativePath.h"
#include <stb_image.h>
#include <fstream>
#include <limits>
#include <vector>
#include <cstring>
#include <memory>

namespace ayt::resource::detail {
struct ImportImage {
    int width = 0, height = 0, sourceChannels = 0;
    std::vector<unsigned char> rgba;
};
inline bool decodeImportImage(const void* bytes, std::size_t size, ImportImage& result)
{
    if (!bytes || !size || size > std::size_t((std::numeric_limits<int>::max)())) return false;
    ImportImage candidate;
    auto* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes), int(size),
        &candidate.width, &candidate.height, &candidate.sourceChannels, STBI_rgb_alpha);
    if (!pixels || candidate.width <= 0 || candidate.height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> owner(pixels, stbi_image_free);
    const auto count = std::size_t(candidate.width) * candidate.height * 4;
    candidate.rgba.assign(pixels, pixels + count);
    result = std::move(candidate);
    return true;
}
inline bool readImportBytes(const std::string& path, std::vector<unsigned char>& bytes)
{
    std::ifstream input(importNativePath(std::filesystem::u8path(path)), std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > (std::numeric_limits<int>::max)()) return false;
    std::vector<unsigned char> candidate(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(candidate.data()), static_cast<std::streamsize>(candidate.size()));
    if (!input) return false;
    bytes = std::move(candidate);
    return true;
}
inline bool readImportImage(const std::string& path, ImportImage& result)
{
    std::vector<unsigned char> bytes;
    return readImportBytes(path, bytes) && decodeImportImage(bytes.data(), bytes.size(), result);
}
}
