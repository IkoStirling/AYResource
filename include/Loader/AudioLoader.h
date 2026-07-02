#pragma once
#include "IAYResourceLoader.h"
#include "assetsImpl/AYAudio.h"
#include <vector>
#include <string>

namespace ayt::resource
{

// ===== AudioLoader — 音频资源加载器 =====
class AudioLoader : public IResourceLoader {
public:
    AudioLoader() = default;

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Audio"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".ayaudio";
};

} // namespace ayt::resource