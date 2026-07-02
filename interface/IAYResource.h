#pragma once
#include "AYSerializer.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_set>

namespace ayt::resource
{

// ============================================================
// ResourceTag - 资源标签系统
// ============================================================
struct ResourceTag {
    std::string name;
    std::string category;

    ResourceTag() = default;
    ResourceTag(const std::string& n, const std::string& cat = {})
        : name(n), category(cat) {}

    bool operator==(const ResourceTag& other) const {
        return name == other.name && category == other.category;
    }
};

struct ResourceTagHash {
    size_t operator()(const ResourceTag& tag) const {
        return std::hash<std::string>()(tag.name + "#" + tag.category);
    }
};

using TagSet = std::unordered_set<ResourceTag, ResourceTagHash>;

// ============================================================
// IResource - 资源基类接口
// ============================================================
class IResource {
public:
    virtual ~IResource() = default;

    // ===== Lifecycle =====
    virtual bool load(const std::string& path) = 0;
    virtual bool unload() = 0;
    virtual bool reload(const std::string& path);

    // ===== Info =====
    virtual size_t sizeInBytes() const = 0;
    virtual bool isLoaded() const;
    virtual void setLoaded(bool loaded) { _loaded = loaded; }
    virtual const std::string& getPath() const;
    virtual const std::string& getType() const;

    // ===== Tags =====
    void addTag(const ResourceTag& tag);
    void removeTag(const ResourceTag& tag);
    bool hasTag(const ResourceTag& tag) const;
    const TagSet& getTags() const;

protected:
    IResource();

    std::string _path;
    std::string _type;
    TagSet _tags;
    bool _loaded = false;

    // Converter 需要访问 protected 成员
    friend class FBXParser;
    friend class GLTFParser;
    friend class MeshConverter;
    friend class MaterialConverter;
    friend class TextureConverter;
    friend class AnimationConverter;
    friend class AudioConverter;
    friend class VideoConverter;
    friend class ShaderConverter;
    friend class FontConverter;
    friend class FontAsset;
    friend class Material;
    friend class Animation;
    friend class Audio;
    friend class Video;
    friend class Script;
    friend class Physics;
};

// ===== Inline implementations =====

inline IResource::IResource() = default;

inline bool IResource::reload(const std::string& path) {
    if (unload()) {
        return load(path);
    }
    return false;
}

inline bool IResource::isLoaded() const {
    return _loaded;
}

inline const std::string& IResource::getPath() const {
    return _path;
}

inline const std::string& IResource::getType() const {
    return _type;
}

inline void IResource::addTag(const ResourceTag& tag) {
    _tags.insert(tag);
}

inline void IResource::removeTag(const ResourceTag& tag) {
    _tags.erase(tag);
}

inline bool IResource::hasTag(const ResourceTag& tag) const {
    return _tags.find(tag) != _tags.end();
}

inline const TagSet& IResource::getTags() const {
    return _tags;
}

} // namespace ayt::resource