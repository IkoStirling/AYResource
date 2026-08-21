#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace ayt::resource
{

// Shared importer/runtime contract.  Texture meaning must come from the
// material slot, never from a filename guess: one source image may legally be
// referenced by more than one slot with different sampling requirements.
enum class TextureColorSpace : unsigned char {
    Linear = 0,
    Srgb,
};

enum class NormalMapY : signed char {
    Negative = -1,
    Positive = 1,
};

// Persisted as the float material property `opacitySource` so every render
// path computes alpha identically.  BaseColorAlpha means there is no
// independent opacity texture; TextureRed is the conventional channel for a
// dedicated grayscale opacity map; TextureAlpha supports explicitly authored
// RGBA opacity maps without guessing in the shader.
enum class MaterialOpacitySource : unsigned char {
    BaseColorAlpha = 0,
    TextureRed = 1,
    TextureAlpha = 2,
};

inline constexpr float materialOpacitySourceValue(MaterialOpacitySource source)
{
    return static_cast<float>(static_cast<unsigned char>(source));
}

// Assimp can expose one FBX image through both Diffuse and Opacity slots.
// Compare the original source references before semantic suffixes (_d/_o) are
// assigned; comparing virtual output paths can never detect that alias.
inline std::string normalizeMaterialTextureSourcePath(std::string_view path)
{
    std::string normalized;
    normalized.reserve(path.size());
    bool previousSlash = false;
    for (const unsigned char byte : path) {
        char ch = static_cast<char>(byte);
        if (ch == '\\') ch = '/';
        if (ch == '/') {
            if (previousSlash) continue;
            previousSlash = true;
        } else {
            previousSlash = false;
            ch = static_cast<char>(std::tolower(byte));
        }
        normalized.push_back(ch);
    }
    while (normalized.size() >= 2 && normalized[0] == '.' && normalized[1] == '/') {
        normalized.erase(0, 2);
    }
    return normalized;
}

inline bool sameMaterialTextureSource(std::string_view left,
                                      std::string_view right)
{
    return !left.empty() && !right.empty()
        && normalizeMaterialTextureSourcePath(left)
            == normalizeMaterialTextureSourcePath(right);
}

struct MaterialTextureContract {
    const char* usageSuffix = "_u";
    TextureColorSpace colorSpace = TextureColorSpace::Linear;
    NormalMapY normalY = NormalMapY::Positive;
};

inline MaterialTextureContract materialTextureContract(std::string_view parameterName)
{
    if (parameterName == "baseColorTexture" || parameterName == "diffuse"
        || parameterName == "albedoMap" || parameterName == "mainTexture") {
        return {"_d", TextureColorSpace::Srgb, NormalMapY::Positive};
    }
    if (parameterName == "emissiveTexture"
        || parameterName == "emissionColorTexture") {
        return {"_e", TextureColorSpace::Srgb, NormalMapY::Positive};
    }
    if (parameterName == "normalTexture" || parameterName == "normalCameraTexture"
        || parameterName == "normalMap") {
        return {"_n", TextureColorSpace::Linear, NormalMapY::Positive};
    }
    if (parameterName == "metallicTexture") {
        return {"_m", TextureColorSpace::Linear, NormalMapY::Positive};
    }
    if (parameterName == "roughnessTexture") {
        return {"_r", TextureColorSpace::Linear, NormalMapY::Positive};
    }
    if (parameterName == "aoTexture") {
        return {"_ao", TextureColorSpace::Linear, NormalMapY::Positive};
    }
    if (parameterName == "opacityTexture" || parameterName == "opacityMap") {
        return {"_o", TextureColorSpace::Linear, NormalMapY::Positive};
    }
    if (parameterName == "heightTexture") {
        return {"_h", TextureColorSpace::Linear, NormalMapY::Positive};
    }
    if (parameterName == "specularTexture") {
        return {"_s", TextureColorSpace::Linear, NormalMapY::Positive};
    }
    if (parameterName == "reflectionTexture") {
        return {"_refl", TextureColorSpace::Linear, NormalMapY::Positive};
    }
    return {};
}

inline bool materialTextureUsesSrgb(std::string_view parameterName)
{
    return materialTextureContract(parameterName).colorSpace == TextureColorSpace::Srgb;
}

} // namespace ayt::resource
