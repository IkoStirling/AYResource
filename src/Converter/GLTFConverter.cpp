#include "Converter\GLTFConverter.h"
#include <Loader\MeshLoader.h>
#include "AYMesh.h"
#include "ayio/File.h"

namespace ayt::resource
{

GLTFConverter::GLTFConverter() = default;

GLTFConverter::GLTFConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void GLTFConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void GLTFConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

static std::string getFileName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

static std::string replaceExt(const std::string& path, const std::string& newExt) {
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return path + newExt;
    }
    return path.substr(0, pos) + newExt;
}

ConversionResult GLTFConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        return result;
    }

    // TODO: 实现 glTF 解析
    // 1. 解析 glTF JSON (或 GLB 二进制格式)
    // 2. 提取网格数据 (accessors, bufferViews, buffers)
    // 3. 处理 primitive 列表
    // 4. 应用转换选项 (MeshOnly 优化)
    // 5. 填充 Mesh 并保存

    // 临时：创建测试数据验证流程
    auto mesh = std::make_shared<Mesh>();
    mesh->_clear();
    mesh->_attributeMask = (1u << static_cast<UInt8>(MeshAttribute::Position));
    mesh->_computeVertexStride();
    mesh->_vertexCount = 8;
    mesh->_indexCount = 36;
    mesh->_vertexData.resize(8 * mesh->_vertexStride);
    mesh->_indices.resize(36);
    mesh->_computeBounds();
    mesh->_loaded = true;

    // 保存到二进制数据
    std::vector<UInt8> binaryData;
    if (!mesh->saveToBinary(binaryData)) {
        return result;
    }

    // 生成输出文件名
    std::string sourceFileName = getFileName(sourcePath);
    std::string outputFileName = replaceExt(sourceFileName, ".aymesh");
    std::string virtualPath = "meshes/" + outputFileName;

    // 写入输出目录
    if (!outputDir.empty()) {
        std::string fullPath = outputDir + "/" + virtualPath;
        if (!ayt::io::File::exists(fullPath)) {
            writeFile(fullPath, binaryData.data(), binaryData.size());
        }
    }

    // 构建资源信息
    ConversionResult::ConvertedResource res;
    res.path = virtualPath;
    res.type = "Mesh";
    res.size = static_cast<int64_t>(binaryData.size());
    result.resources.push_back(res);

    return result;
}

} // namespace ayt::resource