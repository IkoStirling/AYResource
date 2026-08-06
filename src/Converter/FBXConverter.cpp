#include "Converter\FBXConverter.h"
#include "AYVirtualAssetPath.h"
#include "ayio/File.h"
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
    animationConverter.setOutputDir(dir);
}

ConversionResult FBXConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        ayt::log::warn("[FBXConverter] Invalid source path: %s", sourcePath.c_str());
        return result;
    }

    ayt::log::info("[FBXConverter] Starting conversion: %s", sourcePath.c_str());

    // Base name used by Parser (materialSlots) and all typed converters.
    std::string baseName = sourcePath;
    size_t pos = baseName.find_last_of("/\\");
    if (pos != std::string::npos) {
        baseName = baseName.substr(pos + 1);
    }
    pos = baseName.find_last_of('.');
    if (pos != std::string::npos) {
        baseName = baseName.substr(0, pos);
    }

    // 1. Parser 解析源文件
    FBXParser parser(sourcePath);
    parser.setLoadOption(loadOption);
    parser.setSeparateModels(separateModels);
    parser.setAssetBaseName(baseName);
    parser.setTextureUsageSuffix(textureConverter.getUsageSuffix());

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

        // 去重：已处理的纹理 stem
        std::set<std::string> processedTextures;

        for (const auto& mat : asset->materials) {
            for (const auto& texPath : mat.texturePaths) {
                const std::string texName = makeTextureStemFromSourcePath(texPath);

                // 去重：同一纹理被多个材质引用时只处理一次
                if (processedTextures.find(texName) != processedTextures.end()) {
                    continue;
                }
                processedTextures.insert(texName);

                // 转换纹理（虚拟路径 = makeTextureVirtualPath）
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

    // 5c. R-02: 转换 Animation (从 scene->mAnimations 提取)
    if (!asset->animations.empty()) {
        auto animResources = animationConverter.convertAll(asset->animations, baseName);
        for (auto& res : animResources) {
            result.resources.push_back(res);
        }
        ayt::log::info("[FBXConverter] Converted %zu animations", animResources.size());
    }

    // 6. 生成依赖关系 — mesh → materialSlots (already contract paths)
    // MeshConverter sanitizes '/' '\\' in mesh.name to '_'; dep.from must match.
    auto makeMeshVirtualPath = [&](size_t i, const MeshData& mesh) {
        std::string safeName = mesh.name;
        size_t pos;
        while ((pos = safeName.find('/')) != std::string::npos) safeName.replace(pos, 1, "_");
        while ((pos = safeName.find('\\')) != std::string::npos) safeName.replace(pos, 1, "_");
        const std::string name = safeName.empty()
            ? baseName + "_" + std::to_string(i) + ".aymesh"
            : baseName + "_" + safeName + ".aymesh";
        return std::string("meshes/") + name;
    };
    for (size_t i = 0; i < asset->meshes.size(); i++) {
        const auto& mesh = asset->meshes[i];
        const std::string meshPath = makeMeshVirtualPath(i, mesh);
        for (const auto& matSlot : mesh.materialSlots) {
            ConversionResult::Dependency dep;
            dep.from = meshPath;
            dep.to = matSlot;
            result.dependencies.push_back(dep);
        }
    }

    // 7. 材质 → 纹理依赖：优先用 Param.texturePath（与 aymat 内嵌一致），
    // 否则从 texturePaths 推导同一契约路径。
    for (size_t i = 0; i < asset->materials.size(); i++) {
        const auto& mat = asset->materials[i];
        const std::string matPath = makeMaterialVirtualPath(baseName, i);

        std::set<std::string> emitted;
        for (const auto& param : mat.parameters) {
            if (param.type != MaterialParamType::Texture2D &&
                param.type != MaterialParamType::Texture3D &&
                param.type != MaterialParamType::TextureCube) {
                continue;
            }
            if (param.texturePath.empty()) continue;
            if (!emitted.insert(param.texturePath).second) continue;
            ConversionResult::Dependency dep;
            dep.from = matPath;
            dep.to = param.texturePath;
            result.dependencies.push_back(dep);
        }
        for (const auto& texPath : mat.texturePaths) {
            const std::string cooked = makeTextureVirtualPathFromSource(
                texPath, textureConverter.getUsageSuffix());
            if (!emitted.insert(cooked).second) continue;
            ConversionResult::Dependency dep;
            dep.from = matPath;
            dep.to = cooked;
            result.dependencies.push_back(dep);
        }
    }

    // 7b. R-02: 生成 mesh → skeleton, skeleton → animation 依赖关系
    // mesh 路径: meshes/{baseName}_{mesh.name|idx}.aymesh (与 MeshConverter 输出保持一致)
    // skel 路径: skeletons/{baseName}_{Skeleton}.ayskel (与 SkeletonConverter 输出保持一致)
    // anim 路径: animations/{baseName}_{take}.ayanm (与 AnimationConverter 输出保持一致)
    // 注: 仅当有 skin weight 的 mesh 才需要 mesh→skel 边;Phase 0 的 attributeMask 已被 MeshConverter
    // 用上,但 IntermediateAsset::MeshData 是否暴露 attributeMask?目前保守按"任一 mesh 都加边",
    // runtime looseDependency 会按 from 路径匹配 — 多余的边只会被忽略,不会破坏加载。
    if (!asset->skeletons.empty()) {
        std::string skelPath = "skeletons/" + baseName + "_Skeleton.ayskel";

        for (size_t mi = 0; mi < asset->meshes.size(); ++mi) {
            const auto& mesh = asset->meshes[mi];
            const std::string meshPath = makeMeshVirtualPath(mi, mesh);

            ConversionResult::Dependency dep;
            dep.from = meshPath;
            dep.to = skelPath;
            result.dependencies.push_back(dep);
        }

        // skeleton → animation: 每个 anim 可以播放到任何 skeleton,加 (skel, anim) 边
        // 实际运行 AN-01 时会按 anim 自带的 skeleton binding 字段匹配,这里只覆盖 loose-file path
        if (!asset->animations.empty()) {
            // 取所有 animation 的输出 path (从 animationConverter 已生成的 resources 里反向收集)
            // 简化: 用 baseName + "_take_<index>" 重建
            for (size_t ai = 0; ai < asset->animations.size(); ++ai) {
                const auto& anim = asset->animations[ai];
                std::string take = anim.name.empty() ? ("take_" + std::to_string(ai)) : anim.name;
                // 简单 sanitize, 与 AnimationConverter::sanitizeTakeName 保持一致语义
                for (char& c : take) {
                    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
                        || c == '"' || c == '<' || c == '>' || c == '|' || c == ' ' || c == '\t') {
                        c = '_';
                    }
                }
                size_t lead = 0;
                while (lead < take.size() && take[lead] == '.') ++lead;
                if (lead > 0) take.erase(0, lead);

                ConversionResult::Dependency dep;
                dep.from = skelPath;
                dep.to = "animations/" + baseName + "_" + take + ".ayanm";
                result.dependencies.push_back(dep);
            }
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