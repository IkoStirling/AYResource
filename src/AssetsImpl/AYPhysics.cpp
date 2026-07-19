#include "AYPhysics.h"
#include <aymath/MathTypes.h>
#include <ayio/File.h>
#include <cstring>

namespace ayt::resource
{

// ===== Physics 二进制格式 Header =====
#pragma pack(push, 1)
struct PhysicsBinaryHeader {
    UInt32 magic;           // 'AYPH' = 0x48595041
    UInt16 version;         // 版本 = 1
    FGuid guid; // 资源唯一标识 (16 bytes)
    UInt8  shapeType;       // PhysicsShapeType
    UInt8  flags;           // 标志
    float  mass;            // 质量
    UInt32 nameLength;      // 名称长度
    UInt32 shapeDataSize;   // 形状数据字节数
};
#pragma pack(pop)

// ===== Physics =====
Physics::Physics() = default;

void Physics::clear() {
    _name.clear();
    _mass = 1.0f;
    _shapeType = PhysicsShapeType::Box;
    _shapeData.clear();
    _shapeTypeStr.clear();
}

bool Physics::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t Physics::sizeInBytes() const {
    return sizeof(Physics) + _shapeData.size() * sizeof(float) + _name.size();
}

const char* Physics::shapeTypeToString(PhysicsShapeType type) {
    switch (type) {
        case PhysicsShapeType::Box:    return "box";
        case PhysicsShapeType::Sphere:  return "sphere";
        case PhysicsShapeType::Mesh:    return "mesh";
        case PhysicsShapeType::Convex: return "convex";
        default:                       return "unknown";
    }
}

const char* Physics::getShapeType() const {
    return _shapeTypeStr.c_str();
}

void Physics::getBoxHalfExtents(float& x, float& y, float& z) const {
    if (_shapeType == PhysicsShapeType::Box && _shapeData.size() >= 3) {
        x = _shapeData[0];
        y = _shapeData[1];
        z = _shapeData[2];
    } else {
        x = y = z = 0.5f;
    }
}

void Physics::getSphereParams(float& radius, float& cx, float& cy, float& cz) const {
    if (_shapeType == PhysicsShapeType::Sphere && _shapeData.size() >= 4) {
        radius = _shapeData[0];
        cx = _shapeData[1];
        cy = _shapeData[2];
        cz = _shapeData[3];
    } else {
        radius = 0.5f;
        cx = cy = cz = 0.0f;
    }
}

UInt32 Physics::getVertexCount() const {
    if (_shapeType == PhysicsShapeType::Mesh || _shapeType == PhysicsShapeType::Convex) {
        return static_cast<UInt32>(_shapeData.size() / 3);
    }
    return 0;
}

const float* Physics::getVertexData() const {
    if (_shapeType == PhysicsShapeType::Mesh || _shapeType == PhysicsShapeType::Convex) {
        return _shapeData.data();
    }
    return nullptr;
}

bool Physics::load(const std::string& path) {
    _path = path;

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(PhysicsBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Physics::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(PhysicsBinaryHeader)) {
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const PhysicsBinaryHeader* header = reinterpret_cast<const PhysicsBinaryHeader*>(ptr);

    // 验证 magic 和 version
    if (header->magic != IPhysics::MAGIC || header->version != IPhysics::VERSION) {
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取基本属性
    _shapeType = static_cast<PhysicsShapeType>(header->shapeType);
    _mass = header->mass;

    // 读取 name
    size_t offset = sizeof(PhysicsBinaryHeader);
    if (header->nameLength > 0) {
        if (size < offset + header->nameLength) {
            return false;
        }
        _name.assign(reinterpret_cast<const char*>(ptr + offset), header->nameLength);
        offset += header->nameLength;
    } else {
        _name.clear();
    }

    // 读取 shapeData
    if (header->shapeDataSize > 0) {
        if (size < offset + header->shapeDataSize) {
            return false;
        }
        _shapeData.resize(header->shapeDataSize / sizeof(float));
        std::memcpy(_shapeData.data(), ptr + offset, header->shapeDataSize);
        offset += header->shapeDataSize;
    }

    // 读取 shapeTypeStr
    if (offset < size) {
        UInt8 strLen = ptr[offset];
        if (offset + 1 + strLen <= size) {
            _shapeTypeStr.assign(reinterpret_cast<const char*>(ptr + offset + 1), strLen);
        }
    }

    _loaded = true;
    return true;
}

bool Physics::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t nameLen = _name.size();
    size_t shapeDataSize = _shapeData.size() * sizeof(float);
    size_t shapeTypeStrLen = _shapeTypeStr.size();
    size_t totalSize = sizeof(PhysicsBinaryHeader) + nameLen + shapeDataSize + 1 + shapeTypeStrLen;

    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Custom Header
    PhysicsBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = IPhysics::MAGIC;
    header.version = static_cast<UInt16>(IPhysics::VERSION);
    header.guid = _guid;
    header.shapeType = static_cast<UInt8>(_shapeType);
    header.flags = 0;
    header.mass = _mass;
    header.nameLength = static_cast<UInt32>(nameLen);
    header.shapeDataSize = static_cast<UInt32>(shapeDataSize);

    std::memcpy(ptr, &header, sizeof(header));

    // 写入 name
    size_t offset = sizeof(PhysicsBinaryHeader);
    if (nameLen > 0) {
        std::memcpy(ptr + offset, _name.data(), nameLen);
        offset += nameLen;
    }

    // 写入 shapeData
    if (shapeDataSize > 0) {
        std::memcpy(ptr + offset, _shapeData.data(), shapeDataSize);
        offset += shapeDataSize;
    }

    // 写入 shapeTypeStr (长度字节 + 字符串)
    ptr[offset] = static_cast<UInt8>(shapeTypeStrLen);
    if (shapeTypeStrLen > 0) {
        std::memcpy(ptr + offset + 1, _shapeTypeStr.data(), shapeTypeStrLen);
    }

    return true;
}

// ===== 创建测试数据 =====

void Physics::createBox(float hx, float hy, float hz) {
    clear();
    _name = "box";
    _shapeType = PhysicsShapeType::Box;
    _shapeTypeStr = "box";
    _mass = 1.0f;
    _shapeData = {hx, hy, hz};
    _loaded = true;
}

void Physics::createSphere(float radius) {
    clear();
    _name = "sphere";
    _shapeType = PhysicsShapeType::Sphere;
    _shapeTypeStr = "sphere";
    _mass = 1.0f;
    _shapeData = {radius, 0.0f, 0.0f, 0.0f};
    _loaded = true;
}

void Physics::createConvex(const float* vertices, UInt32 vertexCount) {
    clear();
    _name = "convex";
    _shapeType = PhysicsShapeType::Convex;
    _shapeTypeStr = "convex";
    _mass = 1.0f;
    _shapeData.resize(vertexCount * 3);
    std::memcpy(_shapeData.data(), vertices, vertexCount * 3 * sizeof(float));
    _loaded = true;
}

} // namespace ayt::resource