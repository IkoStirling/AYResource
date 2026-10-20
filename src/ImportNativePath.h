#pragma once
#include <filesystem>
#include <string>
namespace ayt::resource::detail {
// Physical Windows I/O may exceed MAX_PATH inside content-addressed caches.
// Keep this spelling private; persisted authoring references remain ordinary UTF-8.
inline std::filesystem::path importNativePath(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const auto native = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
    if (native.rfind(LR"(\\?\)", 0) == 0) return native;
    if (native.rfind(LR"(\\)", 0) == 0) return LR"(\\?\UNC\)" + native.substr(2);
    return LR"(\\?\)" + native;
#else
    return path;
#endif
}
inline std::string importPortablePathText(const std::filesystem::path& path)
{
    auto normal = path;
#if defined(_WIN32)
    const auto& native = path.native();
    if (native.rfind(LR"(\\?\UNC\)", 0) == 0) normal = LR"(\\)" + native.substr(8);
    else if (native.rfind(LR"(\\?\)", 0) == 0) normal = native.substr(4);
#endif
    const auto text = normal.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
}
