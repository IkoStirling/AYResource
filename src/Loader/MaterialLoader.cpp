#include "Loader\MaterialLoader.h"
#include "IAYResourceLoader.h"
#include <ayio/File.h>
#include <aymath/MathTypes.h>
#include <cstring>
#include <cstdio>

namespace ayt::resource
{

// ===== 常量 =====
// ===== 常量 =====

// ===== Material 文件头 (二进制格式) =====
#pragma pack(push, 1)
struct MaterialBinaryHeader {
    UInt32 magic;              // 'AYMT' = 0x544D5941
    UInt16 version;            // 版本 = 1
    FGuid guid;                // 资源唯一标识 (16 bytes)
    UInt8  flags;              // 标志
    UInt8  parameterCount;     // 参数数量
    UInt32 nameLength;         // name 字符串长度
    UInt32 shaderLength;       // shader 字符串长度
    UInt32 dataSize;           // 参数数据总大小
};
#pragma pack(pop)

// ===== Material =====

Material::Material() = default;

void Material::clearImpl() {
    _params.clear();
    _name.clear();
    _shader.clear();
    _path.clear();
}

bool Material::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t Material::sizeInBytes() const {
    size_t size = sizeof(Material);
    for (const auto& pair : _params) {
        size += pair.first.capacity();
        if (pair.second.type >= MaterialParamType::Texture2D &&
            pair.second.type <= MaterialParamType::TextureCube) {
            size += pair.second.stringValue.capacity();
        }
    }
    return size;
}

bool Material::hasParameter(const char* name) const {
    return _params.find(name) != _params.end();
}

MaterialParamType Material::getParameterType(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end()) {
        return MaterialParamType::Float; // default
    }
    return it->second.type;
}

float Material::getFloat(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Float) {
        return 0.0f;
    }
    return it->second.floatValue;
}

void Material::getFloat2(const char* name, float(&out)[2]) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Float2) {
        out[0] = out[1] = 0.0f;
        return;
    }
    out[0] = it->second.float2Value[0];
    out[1] = it->second.float2Value[1];
}

void Material::getFloat3(const char* name, float(&out)[3]) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Float3) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    out[0] = it->second.float3Value[0];
    out[1] = it->second.float3Value[1];
    out[2] = it->second.float3Value[2];
}

void Material::getFloat4(const char* name, float(&out)[4]) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Float4) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    out[0] = it->second.float4Value[0];
    out[1] = it->second.float4Value[1];
    out[2] = it->second.float4Value[2];
    out[3] = it->second.float4Value[3];
}

const char* Material::getTexture(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end()) {
        return "";
    }
    if (it->second.type != MaterialParamType::Texture2D &&
        it->second.type != MaterialParamType::Texture3D &&
        it->second.type != MaterialParamType::TextureCube) {
        return "";
    }
    return it->second.stringValue.c_str();
}

int Material::getInt(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Int) {
        return 0;
    }
    return it->second.intValue;
}

bool Material::getBool(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Bool) {
        return false;
    }
    return it->second.boolValue;
}

ayt::math::FVector2 Material::getVector2(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Float2) {
        return ayt::math::FVector2(0, 0);
    }
    return ayt::math::FVector2(it->second.float2Value[0], it->second.float2Value[1]);
}

ayt::math::FVector3 Material::getVector3(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Float3) {
        return ayt::math::FVector3(0, 0, 0);
    }
    return ayt::math::FVector3(it->second.float3Value[0], it->second.float3Value[1], it->second.float3Value[2]);
}

ayt::math::FVector4 Material::getVector4(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Float4) {
        return ayt::math::FVector4(0, 0, 0, 0);
    }
    return ayt::math::FVector4(it->second.float4Value[0], it->second.float4Value[1],
                            it->second.float4Value[2], it->second.float4Value[3]);
}

ayt::math::FVector4 Material::getColor(const char* name) const {
    return getVector4(name); // Float4 is used for color (RGBA)
}

const Float32* Material::getMatrix(const char* name) const {
    auto it = _params.find(name);
    if (it == _params.end() || it->second.type != MaterialParamType::Float4x4) {
        return nullptr;
    }
    return it->second.matrixValue;
}

void Material::setFloat(const char* name, float value) {
    ParameterValue pv;
    pv.type = MaterialParamType::Float;
    pv.floatValue = value;
    _params[name] = pv;
}

void Material::setFloat2(const char* name, const float value[2]) {
    ParameterValue pv;
    pv.type = MaterialParamType::Float2;
    pv.float2Value[0] = value[0];
    pv.float2Value[1] = value[1];
    _params[name] = pv;
}

void Material::setFloat3(const char* name, const float value[3]) {
    ParameterValue pv;
    pv.type = MaterialParamType::Float3;
    pv.float3Value[0] = value[0];
    pv.float3Value[1] = value[1];
    pv.float3Value[2] = value[2];
    _params[name] = pv;
}

void Material::setFloat4(const char* name, const float value[4]) {
    ParameterValue pv;
    pv.type = MaterialParamType::Float4;
    pv.float4Value[0] = value[0];
    pv.float4Value[1] = value[1];
    pv.float4Value[2] = value[2];
    pv.float4Value[3] = value[3];
    _params[name] = pv;
}

void Material::setTexture(const char* name, const char* path) {
    ParameterValue pv;
    pv.type = MaterialParamType::Texture2D;
    pv.stringValue = path;
    _params[name] = pv;
}

void Material::setInt(const char* name, int value) {
    ParameterValue pv;
    pv.type = MaterialParamType::Int;
    pv.intValue = value;
    _params[name] = pv;
}

void Material::setBool(const char* name, Bool value) {
    ParameterValue pv;
    pv.type = MaterialParamType::Bool;
    pv.boolValue = value;
    _params[name] = pv;
}

void Material::setVector2(const char* name, const ayt::math::FVector2& value) {
    ParameterValue pv;
    pv.type = MaterialParamType::Float2;
    pv.float2Value[0] = value.x;
    pv.float2Value[1] = value.y;
    _params[name] = pv;
}

void Material::setVector3(const char* name, const ayt::math::FVector3& value) {
    ParameterValue pv;
    pv.type = MaterialParamType::Float3;
    pv.float3Value[0] = value.x;
    pv.float3Value[1] = value.y;
    pv.float3Value[2] = value.z;
    _params[name] = pv;
}

void Material::setVector4(const char* name, const ayt::math::FVector4& value) {
    ParameterValue pv;
    pv.type = MaterialParamType::Float4;
    pv.float4Value[0] = value.x;
    pv.float4Value[1] = value.y;
    pv.float4Value[2] = value.z;
    pv.float4Value[3] = value.w;
    _params[name] = pv;
}

void Material::setColor(const char* name, const ayt::math::FVector4& value) {
    setVector4(name, value); // Color is stored as Float4 (RGBA)
}

void Material::setMatrix(const char* name, const Float32* value) {
    if (!value) return;
    ParameterValue pv;
    pv.type = MaterialParamType::Float4x4;
    for (int i = 0; i < 16; i++) {
        pv.matrixValue[i] = value[i];
    }
    _params[name] = pv;
}

bool Material::load(const std::string& path) {
    _path = path;

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(MaterialBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Material::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(MaterialBinaryHeader)) {
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const MaterialBinaryHeader* header = reinterpret_cast<const MaterialBinaryHeader*>(ptr);

    // 验证 magic 和 version
    if (header->magic != IMaterial::MAGIC || header->version != IMaterial::VERSION) {
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取 name
    size_t offset = sizeof(MaterialBinaryHeader);
    if (header->nameLength > 0) {
        if (size < offset + header->nameLength) return false;
        _name.assign(reinterpret_cast<const char*>(ptr + offset), header->nameLength);
        offset += header->nameLength;
    }

    // 读取 shader
    if (header->shaderLength > 0) {
        if (size < offset + header->shaderLength) return false;
        _shader.assign(reinterpret_cast<const char*>(ptr + offset), header->shaderLength);
        offset += header->shaderLength;
    }

    // 读取参数数据
    if (offset < size) {
        size_t dataEnd = offset + header->dataSize;
        if (dataEnd > size) dataEnd = size;

        while (offset < dataEnd) {
            // 读取参数名
            if (offset + sizeof(UInt32) > dataEnd) break;
            UInt32 paramNameLen = *reinterpret_cast<const UInt32*>(ptr + offset);
            offset += sizeof(UInt32);

            if (offset + paramNameLen > dataEnd) break;
            std::string paramName(reinterpret_cast<const char*>(ptr + offset), paramNameLen);
            offset += paramNameLen;

            // 读取类型
            if (offset + sizeof(UInt8) > dataEnd) break;
            UInt8 typeVal = *reinterpret_cast<const UInt8*>(ptr + offset);
            offset += sizeof(UInt8);

            MaterialParamType type = static_cast<MaterialParamType>(typeVal);

            ParameterValue pv;
            pv.type = type;

            switch (type) {
                case MaterialParamType::Float:
                    if (offset + sizeof(Float32) > dataEnd) break;
                    pv.floatValue = *reinterpret_cast<const Float32*>(ptr + offset);
                    offset += sizeof(Float32);
                    break;
                case MaterialParamType::Float2:
                    if (offset + 2 * sizeof(Float32) > dataEnd) break;
                    pv.float2Value[0] = *reinterpret_cast<const Float32*>(ptr + offset);
                    pv.float2Value[1] = *reinterpret_cast<const Float32*>(ptr + offset + 4);
                    offset += 2 * sizeof(Float32);
                    break;
                case MaterialParamType::Float3:
                    if (offset + 3 * sizeof(Float32) > dataEnd) break;
                    pv.float3Value[0] = *reinterpret_cast<const Float32*>(ptr + offset);
                    pv.float3Value[1] = *reinterpret_cast<const Float32*>(ptr + offset + 4);
                    pv.float3Value[2] = *reinterpret_cast<const Float32*>(ptr + offset + 8);
                    offset += 3 * sizeof(Float32);
                    break;
                case MaterialParamType::Float4:
                case MaterialParamType::Float4x4: // Float4x4 uses same storage as Float4 for first 4 floats
                    if (offset + 4 * sizeof(Float32) > dataEnd) break;
                    pv.float4Value[0] = *reinterpret_cast<const Float32*>(ptr + offset);
                    pv.float4Value[1] = *reinterpret_cast<const Float32*>(ptr + offset + 4);
                    pv.float4Value[2] = *reinterpret_cast<const Float32*>(ptr + offset + 8);
                    pv.float4Value[3] = *reinterpret_cast<const Float32*>(ptr + offset + 12);
                    offset += 4 * sizeof(Float32);
                    break;
                case MaterialParamType::Int:
                    if (offset + sizeof(Int32) > dataEnd) break;
                    pv.intValue = *reinterpret_cast<const Int32*>(ptr + offset);
                    offset += sizeof(Int32);
                    break;
                case MaterialParamType::Bool:
                    if (offset + sizeof(Bool) > dataEnd) break;
                    pv.boolValue = *reinterpret_cast<const Bool*>(ptr + offset);
                    offset += sizeof(Bool);
                    break;
                case MaterialParamType::Texture2D:
                case MaterialParamType::Texture3D:
                case MaterialParamType::TextureCube:
                    if (offset + sizeof(UInt32) > dataEnd) break;
                    UInt32 strLen = *reinterpret_cast<const UInt32*>(ptr + offset);
                    offset += sizeof(UInt32);
                    if (offset + strLen > dataEnd) break;
                    pv.stringValue.assign(reinterpret_cast<const char*>(ptr + offset), strLen);
                    offset += strLen;
                    break;
            }

            _params[paramName] = pv;
        }
    }

    _loaded = true;
    return true;
}

bool Material::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算参数数据大小
    size_t paramsDataSize = 0;
    for (const auto& pair : _params) {
        paramsDataSize += sizeof(UInt32) + pair.first.size(); // param name
        paramsDataSize += sizeof(UInt8); // type

        switch (pair.second.type) {
            case MaterialParamType::Float:
                paramsDataSize += sizeof(Float32);
                break;
            case MaterialParamType::Float2:
                paramsDataSize += 2 * sizeof(Float32);
                break;
            case MaterialParamType::Float3:
                paramsDataSize += 3 * sizeof(Float32);
                break;
            case MaterialParamType::Float4:
                paramsDataSize += 4 * sizeof(Float32);
                break;
            case MaterialParamType::Float4x4:
                paramsDataSize += 16 * sizeof(Float32);
                break;
            case MaterialParamType::Int:
                paramsDataSize += sizeof(Int32);
                break;
            case MaterialParamType::Bool:
                paramsDataSize += sizeof(Bool);
                break;
            case MaterialParamType::Texture2D:
            case MaterialParamType::Texture3D:
            case MaterialParamType::TextureCube:
                paramsDataSize += sizeof(UInt32) + pair.second.stringValue.size();
                break;
        }
    }

    // 计算总大小
    size_t totalSize = sizeof(MaterialBinaryHeader) + _name.size() + _shader.size() + paramsDataSize;
    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Custom Header
    MaterialBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = IMaterial::MAGIC;
    header.version = IMaterial::VERSION;
    header.guid = _guid;
    header.flags = 0;
    header.parameterCount = static_cast<UInt8>(_params.size());
    header.nameLength = static_cast<UInt32>(_name.size());
    header.shaderLength = static_cast<UInt32>(_shader.size());
    header.dataSize = static_cast<UInt32>(paramsDataSize);

    std::memcpy(ptr, &header, sizeof(header));

    // 写入 name
    size_t offset = sizeof(MaterialBinaryHeader);
    if (!_name.empty()) {
        std::memcpy(ptr + offset, _name.data(), _name.size());
        offset += _name.size();
    }

    // 写入 shader
    if (!_shader.empty()) {
        std::memcpy(ptr + offset, _shader.data(), _shader.size());
        offset += _shader.size();
    }

    // 写入参数数据
    for (const auto& pair : _params) {
        // param name
        UInt32 nameLen = static_cast<UInt32>(pair.first.size());
        std::memcpy(ptr + offset, &nameLen, sizeof(UInt32));
        offset += sizeof(UInt32);
        if (nameLen > 0) {
            std::memcpy(ptr + offset, pair.first.data(), nameLen);
            offset += nameLen;
        }

        // type
        UInt8 typeVal = static_cast<UInt8>(pair.second.type);
        std::memcpy(ptr + offset, &typeVal, sizeof(UInt8));
        offset += sizeof(UInt8);

        // value
        switch (pair.second.type) {
            case MaterialParamType::Float:
                std::memcpy(ptr + offset, &pair.second.floatValue, sizeof(Float32));
                offset += sizeof(Float32);
                break;
            case MaterialParamType::Float2:
                std::memcpy(ptr + offset, pair.second.float2Value, 2 * sizeof(Float32));
                offset += 2 * sizeof(Float32);
                break;
            case MaterialParamType::Float3:
                std::memcpy(ptr + offset, pair.second.float3Value, 3 * sizeof(Float32));
                offset += 3 * sizeof(Float32);
                break;
            case MaterialParamType::Float4:
                std::memcpy(ptr + offset, pair.second.float4Value, 4 * sizeof(Float32));
                offset += 4 * sizeof(Float32);
                break;
            case MaterialParamType::Float4x4:
                std::memcpy(ptr + offset, pair.second.matrixValue, 16 * sizeof(Float32));
                offset += 16 * sizeof(Float32);
                break;
            case MaterialParamType::Int:
                std::memcpy(ptr + offset, &pair.second.intValue, sizeof(Int32));
                offset += sizeof(Int32);
                break;
            case MaterialParamType::Bool:
                std::memcpy(ptr + offset, &pair.second.boolValue, sizeof(Bool));
                offset += sizeof(Bool);
                break;
            case MaterialParamType::Texture2D:
            case MaterialParamType::Texture3D:
            case MaterialParamType::TextureCube:
                {
                    UInt32 strLen = static_cast<UInt32>(pair.second.stringValue.size());
                    std::memcpy(ptr + offset, &strLen, sizeof(UInt32));
                    offset += sizeof(UInt32);
                    if (strLen > 0) {
                        std::memcpy(ptr + offset, pair.second.stringValue.data(), strLen);
                        offset += strLen;
                    }
                }
                break;
        }
    }

    return true;
}

bool Material::saveToMaterialData(std::vector<UInt8>& outData) const {
    // 计算参数数据大小
    size_t paramsDataSize = 0;
    for (const auto& pair : _params) {
        paramsDataSize += sizeof(UInt32) + pair.first.size();
        paramsDataSize += sizeof(UInt8);

        switch (pair.second.type) {
            case MaterialParamType::Float: paramsDataSize += sizeof(Float32); break;
            case MaterialParamType::Float2: paramsDataSize += 2 * sizeof(Float32); break;
            case MaterialParamType::Float3: paramsDataSize += 3 * sizeof(Float32); break;
            case MaterialParamType::Float4: paramsDataSize += 4 * sizeof(Float32); break;
            case MaterialParamType::Float4x4: paramsDataSize += 16 * sizeof(Float32); break;
            case MaterialParamType::Int: paramsDataSize += sizeof(Int32); break;
            case MaterialParamType::Bool: paramsDataSize += sizeof(Bool); break;
            case MaterialParamType::Texture2D:
            case MaterialParamType::Texture3D:
            case MaterialParamType::TextureCube:
                paramsDataSize += sizeof(UInt32) + pair.second.stringValue.size(); break;
        }
    }

    // 计算总大小：nameLength + name + shaderLength + shader + paramsLength + params
    size_t totalSize = sizeof(UInt32) + _name.size()
                      + sizeof(UInt32) + _shader.size()
                      + sizeof(UInt32) + paramsDataSize;

    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 name
    UInt32 nameLen = static_cast<UInt32>(_name.size());
    std::memcpy(ptr, &nameLen, sizeof(UInt32));
    ptr += sizeof(UInt32);
    if (nameLen > 0) {
        std::memcpy(ptr, _name.data(), nameLen);
        ptr += nameLen;
    }

    // 写入 shader
    UInt32 shaderLen = static_cast<UInt32>(_shader.size());
    std::memcpy(ptr, &shaderLen, sizeof(UInt32));
    ptr += sizeof(UInt32);
    if (shaderLen > 0) {
        std::memcpy(ptr, _shader.data(), shaderLen);
        ptr += shaderLen;
    }

    // 写入参数数据
    UInt32 paramsLen = static_cast<UInt32>(paramsDataSize);
    std::memcpy(ptr, &paramsLen, sizeof(UInt32));
    ptr += sizeof(UInt32);

    for (const auto& pair : _params) {
        // param name
        UInt32 pNameLen = static_cast<UInt32>(pair.first.size());
        std::memcpy(ptr, &pNameLen, sizeof(UInt32));
        ptr += sizeof(UInt32);
        if (pNameLen > 0) {
            std::memcpy(ptr, pair.first.data(), pNameLen);
            ptr += pNameLen;
        }

        // type
        std::memcpy(ptr, &pair.second.type, sizeof(UInt8));
        ptr += sizeof(UInt8);

        // value
        switch (pair.second.type) {
            case MaterialParamType::Float:
                std::memcpy(ptr, &pair.second.floatValue, sizeof(Float32));
                ptr += sizeof(Float32);
                break;
            case MaterialParamType::Float2:
                std::memcpy(ptr, pair.second.float2Value, 2 * sizeof(Float32));
                ptr += 2 * sizeof(Float32);
                break;
            case MaterialParamType::Float3:
                std::memcpy(ptr, pair.second.float3Value, 3 * sizeof(Float32));
                ptr += 3 * sizeof(Float32);
                break;
            case MaterialParamType::Float4:
                std::memcpy(ptr, pair.second.float4Value, 4 * sizeof(Float32));
                ptr += 4 * sizeof(Float32);
                break;
            case MaterialParamType::Float4x4:
                std::memcpy(ptr, pair.second.matrixValue, 16 * sizeof(Float32));
                ptr += 16 * sizeof(Float32);
                break;
            case MaterialParamType::Int:
                std::memcpy(ptr, &pair.second.intValue, sizeof(Int32));
                ptr += sizeof(Int32);
                break;
            case MaterialParamType::Bool:
                std::memcpy(ptr, &pair.second.boolValue, sizeof(Bool));
                ptr += sizeof(Bool);
                break;
            case MaterialParamType::Texture2D:
            case MaterialParamType::Texture3D:
            case MaterialParamType::TextureCube:
                {
                    UInt32 strLen = static_cast<UInt32>(pair.second.stringValue.size());
                    std::memcpy(ptr, &strLen, sizeof(UInt32));
                    ptr += sizeof(UInt32);
                    if (strLen > 0) {
                        std::memcpy(ptr, pair.second.stringValue.data(), strLen);
                        ptr += strLen;
                    }
                }
                break;
        }
    }

    return true;
}

bool Material::loadFromMaterialData(const void* data, size_t size) {
    if (!data || size < 16) return false;  // minimum: nameLen + shaderLen + paramsLen

    clear();
    const UInt8* ptr = static_cast<const UInt8*>(data);
    size_t offset = 0;

    // 读取 name
    UInt32 nameLen = *reinterpret_cast<const UInt32*>(ptr + offset);
    offset += sizeof(UInt32);
    if (nameLen > 0 && offset + nameLen <= size) {
        _name.assign(reinterpret_cast<const char*>(ptr + offset), nameLen);
        offset += nameLen;
    }

    // 读取 shader
    UInt32 shaderLen = *reinterpret_cast<const UInt32*>(ptr + offset);
    offset += sizeof(UInt32);
    if (shaderLen > 0 && offset + shaderLen <= size) {
        _shader.assign(reinterpret_cast<const char*>(ptr + offset), shaderLen);
        offset += shaderLen;
    }

    // 读取参数数据
    UInt32 paramsLen = *reinterpret_cast<const UInt32*>(ptr + offset);
    offset += sizeof(UInt32);
    const size_t paramsEnd = offset + paramsLen;
    if (paramsEnd > size) {
        return false;
    }

    // 解析参数（简化实现，仅解析已知类型）
    while (offset < paramsEnd && offset + 5 <= paramsEnd) {  // min: nameLen(4) + type(1)
        UInt32 pNameLen = *reinterpret_cast<const UInt32*>(ptr + offset);
        offset += sizeof(UInt32);

        std::string pName;
        if (pNameLen > 0 && offset + pNameLen <= size) {
            pName.assign(reinterpret_cast<const char*>(ptr + offset), pNameLen);
            offset += pNameLen;
        }

        if (offset >= size) break;
        MaterialParamType type = static_cast<MaterialParamType>(*(ptr + offset));
        offset += sizeof(UInt8);

        ParameterValue val{};
        val.type = type;

        switch (type) {
            case MaterialParamType::Float:
                if (offset + sizeof(Float32) <= size) {
                    val.floatValue = *reinterpret_cast<const Float32*>(ptr + offset);
                    offset += sizeof(Float32);
                }
                break;
            case MaterialParamType::Float2:
                if (offset + 2 * sizeof(Float32) <= size) {
                    std::memcpy(val.float2Value, ptr + offset, 2 * sizeof(Float32));
                    offset += 2 * sizeof(Float32);
                }
                break;
            case MaterialParamType::Float3:
                if (offset + 3 * sizeof(Float32) <= size) {
                    std::memcpy(val.float3Value, ptr + offset, 3 * sizeof(Float32));
                    offset += 3 * sizeof(Float32);
                }
                break;
            case MaterialParamType::Float4:
                if (offset + 4 * sizeof(Float32) <= size) {
                    std::memcpy(val.float4Value, ptr + offset, 4 * sizeof(Float32));
                    offset += 4 * sizeof(Float32);
                }
                break;
            case MaterialParamType::Float4x4:
                if (offset + 16 * sizeof(Float32) <= size) {
                    std::memcpy(val.matrixValue, ptr + offset, 16 * sizeof(Float32));
                    offset += 16 * sizeof(Float32);
                }
                break;
            case MaterialParamType::Int:
                if (offset + sizeof(Int32) <= size) {
                    val.intValue = *reinterpret_cast<const Int32*>(ptr + offset);
                    offset += sizeof(Int32);
                }
                break;
            case MaterialParamType::Bool:
                if (offset + sizeof(Bool) <= size) {
                    val.boolValue = *reinterpret_cast<const Bool*>(ptr + offset);
                    offset += sizeof(Bool);
                }
                break;
            case MaterialParamType::Texture2D:
            case MaterialParamType::Texture3D:
            case MaterialParamType::TextureCube:
                {
                    UInt32 strLen = 0;
                    if (offset + sizeof(UInt32) <= size) {
                        strLen = *reinterpret_cast<const UInt32*>(ptr + offset);
                        offset += sizeof(UInt32);
                        if (strLen > 0 && offset + strLen <= size) {
                            val.stringValue.assign(reinterpret_cast<const char*>(ptr + offset), strLen);
                            offset += strLen;
                        }
                    }
                }
                break;
        }

        if (!pName.empty()) {
            _params[pName] = val;
        }
    }

    _loaded = true;
    return true;
}

size_t Material::getMaterialDataSize() const {
    size_t size = 0;
    size += sizeof(UInt32) + _name.size();  // name
    size += sizeof(UInt32) + _shader.size();  // shader

    // params size
    size_t paramsSize = 0;
    for (const auto& pair : _params) {
        paramsSize += sizeof(UInt32) + pair.first.size();
        paramsSize += sizeof(UInt8);
        switch (pair.second.type) {
            case MaterialParamType::Float: paramsSize += sizeof(Float32); break;
            case MaterialParamType::Float2: paramsSize += 2 * sizeof(Float32); break;
            case MaterialParamType::Float3: paramsSize += 3 * sizeof(Float32); break;
            case MaterialParamType::Float4: paramsSize += 4 * sizeof(Float32); break;
            case MaterialParamType::Float4x4: paramsSize += 16 * sizeof(Float32); break;
            case MaterialParamType::Int: paramsSize += sizeof(Int32); break;
            case MaterialParamType::Bool: paramsSize += sizeof(Bool); break;
            case MaterialParamType::Texture2D:
            case MaterialParamType::Texture3D:
            case MaterialParamType::TextureCube:
                paramsSize += sizeof(UInt32) + pair.second.stringValue.size(); break;
        }
    }
    size += sizeof(UInt32) + paramsSize;

    return size;
}

void Material::createDefault() {
    clear();
    _name = "Default";
    _shader = "shaders/unlit.phoskia";

    float color[] = {1.0f, 1.0f, 1.0f, 1.0f};
    setFloat4("albedo", color);

    float metallic = 0.0f;
    setFloat("metallic", metallic);

    float smoothness = 0.5f;
    setFloat("smoothness", smoothness);

    float emission[] = {0.0f, 0.0f, 0.0f};
    setFloat3("emission", emission);

    _loaded = true;
}

// ===== MaterialLoader =====

bool MaterialLoader::canLoad(const std::string& path) const {
    if (path.size() < 6) {
        return false;
    }
    return path.compare(path.size() - 6, 6, EXTENSION) == 0;
}

std::shared_ptr<IResource> MaterialLoader::load(const std::string& path) {
    auto material = std::make_shared<Material>();
    if (material->load(path)) {
        return material;
    }
    return nullptr;
}

std::shared_ptr<IResource> MaterialLoader::loadFromBinary(const void* data, size_t size) {
    auto material = std::make_shared<Material>();
    if (material->loadFromBinary(data, size)) {
        return material;
    }
    return nullptr;
}

std::shared_ptr<IResource> MaterialLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto material = load(path);
    if (callback) {
        callback(material);
    }
       return material;
}

} // namespace ayt::resource