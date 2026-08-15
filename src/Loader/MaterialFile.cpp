#include "AYResource/Loader/MaterialFile.h"
#include "AYResource/Loader/MaterialLoader.h"
#include <cstring>

namespace ayt::resource
{

// ===== Material 文件头 (多材质格式) =====
#pragma pack(push, 1)
struct MaterialFileHeader {
    UInt32 magic;              // 'AYMT'
    UInt16 version;            // 版本 = 1
    UInt8  flags;              // 标志
    UInt8  materialCount;       // 材质数量
    UInt32 totalSize;         // 后续所有材质数据的总大小
};
#pragma pack(pop)

bool MaterialFile::saveToBinary(std::vector<UInt8>& outData) const {
    if (_materials.empty()) return false;

    // 计算总大小
    size_t totalMaterialsSize = 0;
    std::vector<std::vector<UInt8>> allMatData;
    allMatData.reserve(_materials.size());

    for (const auto& mat : _materials) {
        std::vector<UInt8> matData;
        if (!mat->saveToMaterialData(matData)) {
            return false;
        }
        totalMaterialsSize += matData.size();
        allMatData.push_back(std::move(matData));
    }

    size_t totalSize = sizeof(MaterialFileHeader) + totalMaterialsSize;
    outData.resize(totalSize);

    // 写入 Header
    MaterialFileHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = 0x544D5941;  // 'AYMT'
    header.version = 1;
    header.flags = 0;
    header.materialCount = static_cast<UInt8>(_materials.size());
    header.totalSize = static_cast<UInt32>(totalMaterialsSize);

    UInt8* ptr = outData.data();
    std::memcpy(ptr, &header, sizeof(header));
    ptr += sizeof(header);

    // 写入每个材质数据段
    for (size_t i = 0; i < allMatData.size(); i++) {
        const auto& matData = allMatData[i];
        std::memcpy(ptr, matData.data(), matData.size());
        ptr += matData.size();
    }

    return true;
}

bool MaterialFile::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(MaterialFileHeader)) {
        return false;
    }

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const MaterialFileHeader* header = reinterpret_cast<const MaterialFileHeader*>(ptr);

    if (header->magic != 0x544D5941) {
        return false;
    }

    ptr += sizeof(MaterialFileHeader);
    size_t remaining = size - sizeof(MaterialFileHeader);

    _materials.clear();

    for (UInt8 i = 0; i < header->materialCount; i++) {
        if (remaining == 0) {
            return false;
        }
        auto mat = std::make_shared<Material>();
        if (!mat->loadFromMaterialData(ptr, remaining)) {
            return false;
        }
        const size_t consumed = mat->getMaterialDataSize();
        if (consumed == 0 || consumed > remaining) {
            return false;
        }
        _materials.push_back(mat);
        ptr += consumed;
        remaining -= consumed;
    }

    return !_materials.empty();
}

size_t MaterialFile::sizeInBytes() const {
    size_t total = sizeof(MaterialFileHeader);
    for (const auto& mat : _materials) {
        total += mat->getMaterialDataSize();
    }
    return total;
}

} // namespace ayt::resource
