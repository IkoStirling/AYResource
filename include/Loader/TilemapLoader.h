#pragma once
#include "IAYResourceLoader.h"
#include "assetsImpl/AYTilemapAsset.h"

namespace ayt::resource
{

// ===== TilemapLoader — `.aytilemap` resource loader =====
class TilemapLoader : public IResourceLoader {
public:
    TilemapLoader() = default;

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Tilemap"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".aytilemap";
};

} // namespace ayt::resource
