#pragma once
#include "AYResource/IResourceLoader.h"
#include "AYResource/assetsImpl/Skeleton.h"

namespace ayt::resource
{

// ===== SkeletonLoader — 骨骼资源加载器 =====
class SkeletonLoader : public IResourceLoader {
public:
    SkeletonLoader();

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Skeleton"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".ayskel";
};

} // namespace ayt::resource