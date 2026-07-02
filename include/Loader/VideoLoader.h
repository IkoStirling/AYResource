#pragma once
#include "IAYResourceLoader.h"
#include "assetsImpl/AYVideo.h"
#include <vector>
#include <string>

namespace ayt::resource
{

// ===== VideoLoader — 视频资源加载器 =====
class VideoLoader : public IResourceLoader {
public:
    VideoLoader() = default;

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Video"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".ayvideo";
};

} // namespace ayt::resource