#pragma once
#include "AYResource/IResourceLoader.h"
#include "AYResource/assetsImpl/FontAsset.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace ayt::resource
{

// ===== FontLoader — 字体资源加载器 =====
class FontLoader : public IResourceLoader {
public:
    FontLoader() = default;

    // ===== IResourceLoader =====
    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Font"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".ayfont";
};

} // namespace ayt::resource