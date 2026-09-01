#pragma once

#include "AYResource/IResourceLoader.h"

namespace ayt::resource
{

class AtlasLoader final : public IResourceLoader {
public:
    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Atlas"; }
    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(
        const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;
};

} // namespace ayt::resource
