#pragma once
#include "AYResource/IResourceLoader.h"
#include "AYResource/assetsImpl/Script.h"

namespace ayt::resource
{

// ===== ScriptLoader — 脚本资源加载器 =====
class ScriptLoader : public IResourceLoader {
public:
    ScriptLoader();

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Script"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".ayscript";
};

} // namespace ayt::resource