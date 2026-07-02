#pragma once
#include "IAYScript.h"
#include "IAYResourceLoader.h"
#include <AYMathTypes.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace ayt::resource
{

// ===== Script — IAYScript 实现类 =====
class Script : public IScript {
public:
    Script();
    virtual ~Script() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;
    size_t sizeInBytes() const override;

    // ===== IAYScript =====
    const char* getName() const override { return _name.c_str(); }
    const char* getLanguage() const override { return _language.c_str(); }
    const char* getSource() const override { return _source.c_str(); }

    // ===== Binary serialization =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== Setters =====
    void setName(const std::string& name) { _name = name; }
    void setLanguage(const std::string& lang) { _language = lang; }
    void setSource(const std::string& source) { _source = source; }

private:
    void clear();

    FGuid _guid;  // 资源唯一标识
    std::string _name;
    std::string _language;
    std::string _source;
    std::string _path;
};

} // namespace ayt::resource