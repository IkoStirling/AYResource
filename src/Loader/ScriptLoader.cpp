#include "Loader\ScriptLoader.h"
#include <fstream>

namespace ayt::resource
{

ScriptLoader::ScriptLoader() = default;

bool ScriptLoader::canLoad(const std::string& path) const {
    if (path.size() < 10) return false;
    return path.compare(path.size() - 10, 10, EXTENSION) == 0;
}

std::shared_ptr<IResource> ScriptLoader::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return nullptr;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<UInt8> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    file.close();

    auto script = std::make_shared<Script>();
    if (!script->loadFromBinary(data.data(), data.size())) {
        return nullptr;
    }

    return script;
}

std::shared_ptr<IResource> ScriptLoader::loadFromBinary(const void* data, size_t size) {
    auto script = std::make_shared<Script>();
    if (!script->loadFromBinary(data, size)) {
        return nullptr;
    }
    return script;
}

std::shared_ptr<IResource> ScriptLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto script = load(path);
    if (callback) {
        callback(script);
    }
    return script;
}

} // namespace ayt::resource