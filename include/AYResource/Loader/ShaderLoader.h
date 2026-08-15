#pragma once
#include "AYResource/IResourceLoader.h"
#include "AYResource/assetsImpl/Shader.h"
#include <vector>
#include <string>

namespace ayt::resource
{

// ===== ShaderLoader — legacy GLSL/HLSL blob (.ayshader)
// Runtime rendering uses Phoskia paths in .aymat; see docs/runtime-conventions.md
class ShaderLoader : public IResourceLoader {
public:
    ShaderLoader() = default;

    bool canLoad(const std::string& path) const override;
    const char* getResourceType() const override { return "Shader"; }

    std::shared_ptr<IResource> load(const std::string& path) override;
    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override;
    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) override;

private:
    static constexpr const char* EXTENSION = ".ayshader";
};

} // namespace ayt::resource