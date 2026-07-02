#pragma once
#include <string>
#include <memory>
#include <functional>
#include "IAYResource.h"
#include "IAYResourceLoader.h"

namespace ayt::resource
{

// Mock resource for testing
class MockResource : public IResource {
public:
    MockResource() {
        _type = "MockResource";
    }

    explicit MockResource(const std::string& path) {
        _path = path;
        _type = "MockResource";
    }

    bool load(const std::string& path) override {
        _path = path;
        _loaded = true;
        return true;
    }

    bool unload() override {
        _loaded = false;
        return true;
    }

    size_t sizeInBytes() const override {
        return 1024;
    }

    void setSize(size_t size) {
        size = size;
    }

private:
    size_t _size = 1024;
};

// Mock loader for testing
class MockResourceLoader : public IResourceLoader {
public:
    MockResourceLoader() = default;

    bool canLoad(const std::string& path) const override {
        return path.find(".mock") != std::string::npos;
    }

    const char* getResourceType() const override {
        return "MockResource";
    }

    std::shared_ptr<IResource> load(const std::string& path) override {
        auto resource = std::make_shared<MockResource>(path);
        resource->load(path);  // Simulate actual loading
        return resource;
    }

    std::shared_ptr<IResource> loadFromBinary(const void* data, size_t size) override {
        auto resource = std::make_shared<MockResource>();
        resource->load("binary loaded");
        // Verify binary data was passed (for testing)
        (void)data;
        (void)size;
        return resource;
    }

    std::shared_ptr<IResource> loadAsync(const std::string& path,
        std::function<void(std::shared_ptr<IResource>)> callback) override {
        auto resource = std::make_shared<MockResource>(path);
        if (callback) {
            callback(resource);
        }
        return resource;
    }
};

} // namespace ayt::resource