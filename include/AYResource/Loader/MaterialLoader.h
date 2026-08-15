#pragma once
#include "AYResource/IResourceLoader.h"
#include "AYResource/assetsImpl/Material.h"

namespace ayt::resource
{

// ===== MaterialLoader — 材质资源加载器 =====
class MaterialLoader : public IResourceLoader {
public:
    MaterialLoader() = default;

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Material"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".aymat";
};

} // namespace ayt::resource