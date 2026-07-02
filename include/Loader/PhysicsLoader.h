#pragma once
#include "IAYResourceLoader.h"
#include "assetsImpl/AYPhysics.h"

namespace ayt::resource
{

// ===== PhysicsLoader — 物理资源加载器 =====
class PhysicsLoader : public IResourceLoader {
public:
    PhysicsLoader() = default;

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Physics"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".ayphys";
};

} // namespace ayt::resource