#pragma once
#include "IAYResourceLoader.h"
#include "assetsImpl/AYSkeletonMask.h"

namespace ayt::resource
{

// ===== SkeletonMaskLoader — 骨骼遮罩资源加载器 =====
//
// P3.x 刀 1 (2026-08-06) — .aymask v1 binary loader. Mirrors
// SkeletonLoader / AnimationLoader pattern: 50-line skeleton that
// delegates to SkeletonMask::load / loadFromBinary.
//
// .aymask v1 binary format defined in
// AYResource/src/AssetsImpl/AYSkeletonMask.cpp.
class SkeletonMaskLoader : public IResourceLoader {
public:
    SkeletonMaskLoader();

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "SkeletonMask"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".aymask";
};

} // namespace ayt::resource