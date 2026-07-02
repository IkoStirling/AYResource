#include "AYScript.h"
#include <AYMathTypes.h>
#include <AYFile.h>
#include <cstring>
#include <cstdio>

namespace ayt::resource
{

// ===== 二进制格式头 =====
#pragma pack(push, 1)
struct ScriptBinaryHeader {
    UInt32 magic;          // 'AYSC' = 0x43415941
    UInt32 version;        // 版本 = 1
    FGuid guid;            // 资源唯一标识 (16 bytes)
    UInt32 nameLength;     // name 长度
    UInt32 languageLength; // language 长度
    UInt32 sourceLength;   // source 长度
    // 后面跟着: name, language, source (连续内存)
};
#pragma pack(pop)

static constexpr size_t SCRIPT_HEADER_SIZE = sizeof(ScriptBinaryHeader);

// ===== Script =====

Script::Script() = default;

void Script::clear() {
    _name.clear();
    _language.clear();
    _source.clear();
}

bool Script::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t Script::sizeInBytes() const {
    return sizeof(Script) + _name.size() + _language.size() + _source.size();
}

bool Script::load(const std::string& path) {
    _path = path;

    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < SCRIPT_HEADER_SIZE) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Script::loadFromBinary(const void* data, size_t size) {
    if (!data || size < SCRIPT_HEADER_SIZE) {
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const ScriptBinaryHeader* header = reinterpret_cast<const ScriptBinaryHeader*>(ptr);

    // 验证 magic
    if (header->magic != IScript::MAGIC) {
        return false;
    }

    // 验证版本
    if (header->version != IScript::VERSION) {
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取数据
    UInt32 offset = SCRIPT_HEADER_SIZE;

    // 读取 name
    if (header->nameLength > 0) {
        if (offset + header->nameLength > size) {
            return false;
        }
        _name = std::string(reinterpret_cast<const char*>(ptr + offset), header->nameLength);
        offset += header->nameLength;
    }

    // 读取 language
    if (header->languageLength > 0) {
        if (offset + header->languageLength > size) {
            return false;
        }
        _language = std::string(reinterpret_cast<const char*>(ptr + offset), header->languageLength);
        offset += header->languageLength;
    }

    // 读取 source
    if (header->sourceLength > 0) {
        if (offset + header->sourceLength > size) {
            return false;
        }
        _source = std::string(reinterpret_cast<const char*>(ptr + offset), header->sourceLength);
        offset += header->sourceLength;
    }

    _loaded = true;
    return true;
}

bool Script::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t totalSize = SCRIPT_HEADER_SIZE;
    totalSize += _name.size();
    totalSize += _language.size();
    totalSize += _source.size();

    // 分配输出缓冲区
    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Header
    ScriptBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = IScript::MAGIC;
    header.version = IScript::VERSION;
    header.guid = _guid;
    header.nameLength = static_cast<UInt32>(_name.size());
    header.languageLength = static_cast<UInt32>(_language.size());
    header.sourceLength = static_cast<UInt32>(_source.size());

    std::memcpy(ptr, &header, sizeof(header));
    UInt32 offset = SCRIPT_HEADER_SIZE;

    // 写入 name
    if (_name.size() > 0) {
        std::memcpy(ptr + offset, _name.data(), _name.size());
        offset += static_cast<UInt32>(_name.size());
    }

    // 写入 language
    if (_language.size() > 0) {
        std::memcpy(ptr + offset, _language.data(), _language.size());
        offset += static_cast<UInt32>(_language.size());
    }

    // 写入 source
    if (_source.size() > 0) {
        std::memcpy(ptr + offset, _source.data(), _source.size());
        offset += static_cast<UInt32>(_source.size());
    }

    return true;
}

} // namespace ayt::resource