#pragma once
#include <string>
#include <memory>
#include <unordered_map>
#include "IAYResourceLoader.h"

namespace ayt::resource
{

// ============================================================
// ResourceRegistry - 类型注册表
// ============================================================
class ResourceRegistry {
public:
    ResourceRegistry() = delete;
    ~ResourceRegistry() = delete;

    // ===== Type registration =====
    static bool registerLoader(const std::string& type, LoaderCreator creator);
    static bool unregisterLoader(const std::string& type);
    static std::unique_ptr<IResourceLoader> createLoader(const std::string& type);

    // ===== Path-based loading =====
    static std::string getTypeFromPath(const std::string& path);
    static std::shared_ptr<IResource> loadByPath(const std::string& path);

    // ===== Extension mapping =====
    static bool registerExtension(const std::string& ext, const std::string& type);
    static const std::string& getTypeFromExtension(const std::string& ext);

private:
    static std::unordered_map<std::string, LoaderCreator>& getLoaders();
    static std::unordered_map<std::string, std::string>& getExtensionMap();
};

inline bool ResourceRegistry::registerExtension(const std::string& ext, const std::string& type) {
    getExtensionMap()[ext] = type;
    return true;
}

inline const std::string& ResourceRegistry::getTypeFromExtension(const std::string& ext) {
    static const std::string kEmpty;
    auto it = getExtensionMap().find(ext);
    return it != getExtensionMap().end() ? it->second : kEmpty;
}

} // namespace ayt::resource