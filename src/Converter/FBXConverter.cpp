#include "Converter\FBXConverter.h"
#include "AYFile.h"
#include <AYLog.h>
#include <sstream>
#include <set>

namespace ayt::resource
{

// 写入二进制文件
static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

FBXConverter::FBXConverter() = default;

FBXConverter::FBXConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void FBXConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void FBXConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
    meshConverter.setOutputDir(dir);
    materialConverter.setOutputDir(dir);
    textureConverter.setOutputDir(dir);
    skeletonConverter.setOutputDir(dir);
}

ConversionResult FBXConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        ayt::log::warn("[FBXConverter] Invalid source path: %s", sourcePath.c_str());
        return result;
    }

    ayt::log::info("[FBXConverter] Starting conversion: %s", sourcePath.c_str());

    // 1. Parser 解析源文件
    FBXParser parser(sourcePath);
    parser.setLoadOption(loadOption);
    parser.setSeparateModels(separateModels);

    if (!parser.parse(sourcePath)) {
        ayt::log::error("[FBXConverter] Failed to parse FBX: %s", sourcePath.c_str());
        return result;
    }

    auto asset = parser.getResult();
    if (!asset) {
        ayt::log::error("[FBXConverter] Parser returned null asset: %s", sourcePath.c_str());
        return result;
    }

    ayt::log::info("[FBXConverter] Parsed FBX - meshes=%zu materials=%zu textures=%zu skeletons=%zu",
             asset->meshes.size(), asset->materials.size(),
             asset->textures.size(), asset->skeletons.size());

    //2. 生成基础文件名
    std::string baseName = sourcePath;
    size_t pos = baseName.find_last_of("/\\");
    if (pos != std::string::npos) {
        baseName = baseName.substr(pos + 1);
    }
    pos = baseName.find_last_of('.');
    if (pos != std::string::npos) {
        baseName = baseName.substr(0, pos);
    }

    // 3. 转换 Mesh
    if (!asset->meshes.empty()) {
        auto meshResources = meshConverter.convertAll(asset->meshes, baseName);
        for (auto& res : meshResources) {
            result.resources.push_back(res);
        }
        ayt::log::info("[FBXConverter] Converted %zu meshes", meshResources.size());
    }

    // 4. 转换 Material
    if (!asset->materials.empty()) {
        auto matResources = materialConverter.convertAll(asset->materials, baseName);
        for (auto& res : matResources) {
            result.resources.push_back(res);
        }
        ayt::log::info("[FBXConverter] Converted %zu materials", matResources.size());
    }

    // 4b. 转换材质引用的外部纹理（从 texturePaths）
    {
        // 获取 FBX 所在目录，用于解析相对路径
        std::string fbxDir;
        size_t lastSlash = sourcePath.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            fbxDir = sourcePath.substr(0, lastSlash);
        }

        // 去重：已处理的纹理路径
        std::set<std::string> processedTextures;

        for (const auto& mat : asset->materials) {
            for (const auto& texPath : mat.texturePaths) {
                // 生成纹理名称：保留子目录，子目录与文件名之间用下划线分隔
                // 例如 "tex/skin.png" → texName = "tex_skin"
                std::string texName = texPath;
                size_t dotPos = texName.find_last_of('.');
                if (dotPos != std::string::npos) {
                    texName = texName.substr(0, dotPos);
                }
                // 保留子目录，只把最后的路径分隔符替换为下划线
                size_t lastSep = texName.find_last_of("/\\");
                if (lastSep != std::string::npos) {
                    texName = texName.substr(0, lastSep) + "_" + texName.substr(lastSep + 1);
                }

                // 去重：同一纹理被多个材质引用时只处理一次
                if (processedTextures.find(texName) != processedTextures.end()) {
                    continue;
                }
                processedTextures.insert(texName);

                // 转换纹理
                auto texResult = textureConverter.convertFromPath(texPath, texName, fbxDir);
                for (auto& res : texResult.resources) {
                    result.resources.push_back(res);
                }
            }
        }
    }

    // 5. 转换 Texture（embedded 纹理）
    if (!asset->textures.empty()) {
        auto texResources = textureConverter.convertAll(asset->textures, baseName);
        for (auto& res : texResources) {
            result.resources.push_back(res);
        }
    }

    // 5b. 转换 Skeleton
    if (!asset->skeletons.empty()) {
        auto skelResources = skeletonConverter.convertAll(asset->skeletons, baseName);
        for (auto& res : skelResources) {
            result.resources.push_back(res);
        }
    }

    // 6. 生成依赖关系
    for (size_t i = 0; i < asset->meshes.size(); i++) {
        const auto& mesh = asset->meshes[i];
        for (const auto& matSlot : mesh.materialSlots) {
            ConversionResult::Dependency dep;
            // 从 mesh资源路径找到对应的 output path
            std::string meshPath = "meshes/" + (mesh.name.empty()
                ? baseName + "_" + std::to_string(i) + ".aymesh"
                : baseName + "_" + mesh.name + ".aymesh");
            dep.from = meshPath;
            dep.to = matSlot;
            result.dependencies.push_back(dep);
        }
    }

    // 7. 生成材质 → 纹理依赖关系
    for (size_t i = 0; i < asset->materials.size(); i++) {
        const auto& mat = asset->materials[i];
        std::string matPath = "materials/" + baseName + "_material_" + std::to_string(i) + ".aymat";

        for (const auto& texPath : mat.texturePaths) {
            // 生成纹理名称（与 section 4b 保持一致）
            std::string texName = texPath;
            size_t dotPos = texName.find_last_of('.');
            if (dotPos != std::string::npos) {
                texName = texName.substr(0, dotPos);
            }
            // 保留子目录，只把最后的路径分隔符替换为下划线
            size_t lastSep = texName.find_last_of("/\\");
            if (lastSep != std::string::npos) {
                texName = texName.substr(0, lastSep) + "_" + texName.substr(lastSep + 1);
            }

            ConversionResult::Dependency dep;
            dep.from = matPath;
            dep.to = "textures/" + texName + textureConverter.getUsageSuffix() + ".aytex";
            result.dependencies.push_back(dep);
        }
    }

    // 8. 写入依赖文件
    if (!outputDir.empty()) {
        std::string depFilePath = outputDir + "/" + baseName + ".aydep.json";
        std::string jsonContent = result.toJson();
        writeFile(depFilePath, jsonContent.data(), jsonContent.size());
    }

    ayt::log::info("[FBXConverter] Conversion complete - resources=%zu dependencies=%zu",
             result.resources.size(), result.dependencies.size());

    return result;
}

} // namespace ayt::resource