#pragma once
#include "AYResource/IResource.h"
#include "AYResource/assetsDefs/IShader.h"
#include <AYMath/MathTypes.h>
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{
// ===== Shader — IShader 实现类 =====
class Shader : public IShader {
public:
    Shader();
    virtual ~Shader() = default;

    // ===== IResource =====
    bool load(const std::string& path) override;
    bool unload() override;

    // ===== IAYShader / IShader =====
    const char* getName() const override { return _name.c_str(); }
    const char* getSource() const override { return _source.c_str(); }
    const char* getEntryPoint() const override { return _entryPoint.c_str(); }
    const char* getProfile() const override { return _profile.c_str(); }

    size_t sizeInBytes() const override;

    // ===== 二进制序列化 =====
    bool loadFromBinary(const void* data, size_t size);
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // ===== GUID =====
    const FGuid& getGuid() const { return _guid; }
    void setGuid(const FGuid& guid) { _guid = guid; }

    // ===== 属性访问 (用于 Converter/Loader) =====
    const std::string& getNameStr() const { return _name; }
    const std::string& getSourceStr() const { return _source; }
    const std::string& getEntryPointStr() const { return _entryPoint; }
    const std::string& getProfileStr() const { return _profile; }

    void setName(const std::string& name) { _name = name; }
    void setSource(const std::string& source) { _source = source; }
    void setEntryPoint(const std::string& entry) { _entryPoint = entry; }
    void setProfile(const std::string& profile) { _profile = profile; }

private:
    FGuid _guid;  // 资源唯一标识
    std::string _name;
    std::string _source;
    std::string _entryPoint;
    std::string _profile;
    std::string _path;
};

} // namespace ayt::resource