#pragma once
#include "AYResource/Loader/MaterialLoader.h"
#include <vector>
#include <memory>

namespace ayt::resource
{

// ===== MaterialFile — 多材质文件封装 =====
// 一个 .aymat 文件可包含多个材质，按索引访问
class MaterialFile {
public:
    MaterialFile() = default;

    // 添加材质
    void addMaterial(std::shared_ptr<Material> material) {
        _materials.push_back(material);
    }

    // 获取材质数量
    size_t getMaterialCount() const { return _materials.size(); }

    // 获取指定索引的材质
    std::shared_ptr<Material> getMaterial(size_t index) {
        if (index < _materials.size()) {
            return _materials[index];
        }
        return nullptr;
    }

    // 保存到二进制（多材质格式）
    bool saveToBinary(std::vector<UInt8>& outData) const;

    // 从二进制加载
    bool loadFromBinary(const void* data, size_t size);

    // 获取总大小
    size_t sizeInBytes() const;

private:
    std::vector<std::shared_ptr<Material>> _materials;
};

} // namespace ayt::resource
