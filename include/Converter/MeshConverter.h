#pragma once
#include "AYIntermediateAsset.h"
#include "IAYConverter.h"
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== MeshConverter — MeshData → .aymesh =====
// 可复用的 Mesh 转换器，FBX/glTF 共用
class MeshConverter {
public:
    MeshConverter() = default;

    // 设置输出目录
    void setOutputDir(const std::string& dir) { outputDir = dir; }

    // 设置虚拟路径前缀
    void setVirtualPath(const std::string& path) { virtualPath = path; }

    // 单个 Mesh 转换
    bool convert(const MeshData& mesh);

    // 批量转换
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<MeshData>& meshes,
        const std::string& baseName
    );

    // 获取最后一个转换的资源路径
    const std::string& getLastOutputPath() const { return lastOutputPath; }

private:
    std::string outputDir;
    std::string virtualPath;
    std::string lastOutputPath;
    ayt::math::FGuid lastGuid;

    // 计算顶点 stride
    static UInt8 computeVertexStride(uint8_t attributeMask);

    // 保存到二进制
    bool saveToBinary(const MeshData& mesh, std::vector<UInt8>& outData);
};

} // namespace ayt::resource