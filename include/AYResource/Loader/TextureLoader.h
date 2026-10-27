#pragma once
#include "AYResource/IResourceLoader.h"
#include "AYResource/assetsImpl/Texture.h"
#include <vector>
#include <string>

namespace ayt::resource
{

// ===== TextureLoader — 纹理资源加载器 =====
class TextureLoader : public IResourceLoader {
public:
    TextureLoader() = default;

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Texture"; }

    /// Loads cooked textures or an explicitly selected raw image path. In
    /// Release, extension registration stays disabled, so a database type
    /// or direct loader call must select raw images.
    std::shared_ptr<IResource> load(const std::string& path) override;
    /// Decode a cooked Texture envelope, or an explicitly typed package
    /// PNG/JPEG/BMP/TGA image as one RGBA8 mip. This does not enable loose
    /// extension discovery in Release.
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

};

} // namespace ayt::resource
