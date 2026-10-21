#pragma once
#include <cstdint>

namespace ayt::resource {

enum class TextureFilter : std::uint8_t { None = 0, Point, Bilinear, Trilinear, Anisotropic };
enum class TextureWrap : std::uint8_t { Repeat = 0, Clamp, Mirror, Border };

/// Authored sampling independent of a device/resource owner. AYRenderer reports
/// explicit fallbacks for unsupported fields; numeric anisotropy, border color
/// and LOD bias are device/backend contracts.
struct TextureSampler {
    TextureFilter minFilter = TextureFilter::Bilinear;
    TextureFilter magFilter = TextureFilter::Bilinear;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    TextureWrap wrapW = TextureWrap::Repeat;
    float anisotropy = 1.0f;
    bool sRGB = false;
    float lodBias = 0.0f;
};

} // namespace ayt::resource
