#pragma once
#include "IAYResourceLoader.h"
#include "assetsImpl/AYAnimation.h"

namespace ayt::resource
{

// ===== AnimationLoader — 动画资源加载器 =====
class AnimationLoader : public IResourceLoader {
public:
    AnimationLoader();

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Animation"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".ayanm";
};

} // namespace ayt::resource