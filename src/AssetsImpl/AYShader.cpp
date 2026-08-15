#include "AYResource/assetsImpl/Shader.h"
#include <AYMath/MathTypes.h>
#include <AYIO/File.h>
#include <cstring>

namespace ayt::resource
{

// ===== Shader 文件头 (二进制格式) =====
#pragma pack(push, 1)
struct ShaderBinaryHeader {
    UInt32 magic;          // 'AYSH' = 0x48595341
    UInt32 version;         // 版本 = 1
    FGuid guid;            // 资源唯一标识 (16 bytes)
    UInt32 nameLength;      // name 字符串长度
    UInt32 sourceLength;    // source 字符串长度
    UInt32 entryPointLength;// entryPoint 字符串长度
    UInt32 profileLength;   // profile 字符串长度
    // 后面跟着: name[0..nameLength-1], source[], entryPoint[], profile[]
};
#pragma pack(pop)

// ===== Shader =====

Shader::Shader() = default;

bool Shader::unload() {
    _name.clear();
    _source.clear();
    _entryPoint.clear();
    _profile.clear();
    _loaded = false;
    return true;
}

size_t Shader::sizeInBytes() const {
    return sizeof(Shader) + _name.size() + _source.size() + _entryPoint.size() + _profile.size();
}

bool Shader::load(const std::string& path) {
    _path = path;

    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(ShaderBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Shader::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(ShaderBinaryHeader)) {
        return false;
    }

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const ShaderBinaryHeader* header = reinterpret_cast<const ShaderBinaryHeader*>(ptr);

    // 验证 magic
    if (header->magic != IShader::MAGIC) {
        return false;
    }

    // 验证版本
    if (header->version != IShader::VERSION) {
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 验证字符串长度
    UInt32 totalStringsLength = header->nameLength + header->sourceLength +
                                 header->entryPointLength + header->profileLength;
    UInt32 headerSize = sizeof(ShaderBinaryHeader);
    if (headerSize + totalStringsLength > size) {
        return false;
    }

    // 读取 name
    _name.assign(reinterpret_cast<const char*>(ptr + headerSize), header->nameLength);

    // 读取 source
    size_t offset = headerSize + header->nameLength;
    _source.assign(reinterpret_cast<const char*>(ptr + offset), header->sourceLength);

    // 读取 entryPoint
    offset += header->sourceLength;
    _entryPoint.assign(reinterpret_cast<const char*>(ptr + offset), header->entryPointLength);

    // 读取 profile
    offset += header->entryPointLength;
    _profile.assign(reinterpret_cast<const char*>(ptr + offset), header->profileLength);

    _loaded = true;
    return true;
}

bool Shader::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t totalSize = sizeof(ShaderBinaryHeader) + _name.size() + _source.size() +
                        _entryPoint.size() + _profile.size();

    // 分配输出缓冲区
    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Header
    ShaderBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = IShader::MAGIC;
    header.version = IShader::VERSION;
    header.guid = _guid;
    header.nameLength = static_cast<UInt32>(_name.size());
    header.sourceLength = static_cast<UInt32>(_source.size());
    header.entryPointLength = static_cast<UInt32>(_entryPoint.size());
    header.profileLength = static_cast<UInt32>(_profile.size());

    std::memcpy(ptr, &header, sizeof(header));

    // 写入字符串数据
    size_t offset = sizeof(ShaderBinaryHeader);

    if (!_name.empty()) {
        std::memcpy(ptr + offset, _name.data(), _name.size());
        offset += _name.size();
    }

    if (!_source.empty()) {
        std::memcpy(ptr + offset, _source.data(), _source.size());
        offset += _source.size();
    }

    if (!_entryPoint.empty()) {
        std::memcpy(ptr + offset, _entryPoint.data(), _entryPoint.size());
        offset += _entryPoint.size();
    }

    if (!_profile.empty()) {
        std::memcpy(ptr + offset, _profile.data(), _profile.size());
    }

    return true;
}

} // namespace ayt::resource
