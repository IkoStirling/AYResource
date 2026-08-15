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

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".aytex";
};

} // namespace ayt::resource