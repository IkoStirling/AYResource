#pragma once
#include "AYResource/IResourceLoader.h"


namespace ayt::resource
{

// ===== MeshLoader — 网格资源加载器 =====
class MeshLoader : public IResourceLoader {
public:
    MeshLoader() = default;

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Mesh"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".aymesh";
};

} // namespace ayt::resource