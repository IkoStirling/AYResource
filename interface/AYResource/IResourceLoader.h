#pragma once
#include <memory>
#include <string>
#include <functional>

namespace ayt::resource
{

class IResource;

// ============================================================
// IResourceLoader - 资源加载器接口
// ============================================================
//
// 所有权语义：
// - load() / loadFromBinary() 返回 shared_ptr<IResource>，调用者获得共享所有权
// - Loader 内部创建一个 shared_ptr，调用者与 Loader 共享此资源
// - 调用者可以自行保管 resource，或将其交给 ResourceManager 缓存
// - 资源生命周期由 shared_ptr 引用计数管理，无人持有时自动释放
//
class IResourceLoader {
public:
    virtual ~IResourceLoader() = default;

    // 检查是否支持此路径
    virtual bool canLoad(const std::string& path) const = 0;
    // 获取资源类型标识
    virtual const char* getResourceType() const = 0;
    // 同步加载，失败返回 nullptr
    // @return shared_ptr<IResource>，调用者获得共享所有权
    virtual std::shared_ptr<IResource> load(const std::string& path) = 0;
    // 从二进制数据加载，失败返回 nullptr
    // @return shared_ptr<IResource>，调用者获得共享所有权
    virtual std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) = 0;
    // 异步加载
    // @param callback 加载完成回调
    // @return shared_ptr<IResource>，调用者获得共享所有权
    virtual std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback = {}) = 0;
};

using LoaderCreator = std::unique_ptr<IResourceLoader>(*)();

} // namespace ayt::resource
