#include "AYResource/Converter/FBXConverter.h"
#include "AYResource/MaterialTextureContract.h"
#include "AYResource/MaterialSurfaceClassifier.h"
#include "AYResource/VirtualAssetPath.h"
#include "AYIO/File.h"
#include <AYLog.h>
#include <stb_image.h>
#include <sstream>
#include <set>
#include <cstdlib>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <memory>

namespace ayt::resource
{

MaterialAlphaMode classifyMaterialAlphaCoverage(
    const MaterialAlphaCoverage& coverage,
    MaterialAlphaEvidence evidence)
{
    if (coverage.sampleCount == 0) {
        return MaterialAlphaMode::Opaque;
    }

    const std::size_t nonOpaque =
        coverage.transparentCount + coverage.partialCount;
    // Ignore isolated interpolation/noise samples (at most 0.1%).
    if (nonOpaque * 1000 <= coverage.sampleCount) {
        return MaterialAlphaMode::Opaque;
    }

    // Binary cutouts can contain a small antialiased fringe.  Continuous
    // alpha is Blend only when it forms more than 10% of non-opaque evidence.
    if (coverage.partialCount * 10 <= nonOpaque) {
        return MaterialAlphaMode::Mask;
    }
    // Base-color alpha is ambiguous in FBX: exporters frequently preserve an
    // RGBA atlas even when the DCC material itself is Opaque/Hashed.  Routing
    // such a surface through Blend removes depth writes and breaks occlusion
    // inside a single submesh.  Only a distinct authored opacity input may
    // infer true blending without an explicit scalar/source blend flag.
    return evidence == MaterialAlphaEvidence::DedicatedOpacity
        ? MaterialAlphaMode::Blend
        : MaterialAlphaMode::Mask;
}

float inferMaterialAlphaCutoff(const MaterialAlphaCoverage& coverage,
                               MaterialAlphaMode mode) noexcept
{
    if (mode != MaterialAlphaMode::Mask) {
        return 0.5f;
    }
    // sampleAlpha classifies [0, 8] as transparent. When no referenced
    // sample is in that range, 0.5 would turn an authored continuous-alpha
    // garment into holes. Keep every meaningful non-zero texel instead.
    if (coverage.transparentCount == 0 && coverage.partialCount != 0) {
        return 9.0f / 255.0f;
    }
    return 0.5f;
}

namespace {

struct DecodedImage {
    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    std::vector<unsigned char> rgba;
};

const char* alphaModeName(MaterialAlphaMode mode);

std::string resolveTextureSourcePath(const std::string& source,
                                     const std::string& sourceDirectory)
{
    namespace fs = std::filesystem;
    if (source.empty() || source[0] == '*') {
        return {};
    }
    fs::path path(source);
    if (path.is_relative()) {
        path = fs::path(sourceDirectory) / path;
    }
    return path.lexically_normal().string();
}

std::shared_ptr<const DecodedImage> decodeImageCached(
    const std::string& path,
    std::unordered_map<std::string, std::shared_ptr<const DecodedImage>>& cache)
{
    const auto found = cache.find(path);
    if (found != cache.end()) {
        return found->second;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(
        path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        cache.emplace(path, nullptr);
        return {};
    }

    auto image = std::make_shared<DecodedImage>();
    image->width = width;
    image->height = height;
    image->sourceChannels = channels;
    image->rgba.assign(
        pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    cache.emplace(path, image);
    return image;
}

const MaterialData::TextureSource* findTextureSource(
    const MaterialData& material, const char* parameterName)
{
    const auto found = std::find_if(
        material.textureSources.begin(), material.textureSources.end(),
        [parameterName](const MaterialData::TextureSource& source) {
            return source.parameterName == parameterName;
        });
    return found == material.textureSources.end() ? nullptr : &*found;
}

bool addressUv(float value, MaterialTextureWrap mode, float& addressed)
{
    switch (mode) {
    case MaterialTextureWrap::Clamp:
        addressed = std::clamp(value, 0.0f, 1.0f);
        return true;
    case MaterialTextureWrap::Mirror: {
        float period = std::fmod(value, 2.0f);
        if (period < 0.0f) period += 2.0f;
        addressed = period <= 1.0f ? period : 2.0f - period;
        return true;
    }
    case MaterialTextureWrap::Decal:
        if (value < 0.0f || value > 1.0f) return false;
        addressed = value;
        return true;
    case MaterialTextureWrap::Wrap:
    default:
        addressed = value - std::floor(value);
        return true;
    }
}

void sampleAlpha(MaterialAlphaCoverage& coverage, const DecodedImage& image,
                 float u, float v, bool useRedChannel,
                 const MaterialData::TextureSource& binding)
{
    if (binding.hasUvTransform) {
        u *= binding.uvScale[0];
        v *= binding.uvScale[1];
        const float centeredU = u - 0.5f;
        const float centeredV = v - 0.5f;
        const float cosine = std::cos(binding.uvRotation);
        const float sine = std::sin(binding.uvRotation);
        u = centeredU * cosine - centeredV * sine + 0.5f
            + binding.uvTranslation[0];
        v = centeredU * sine + centeredV * cosine + 0.5f
            + binding.uvTranslation[1];
    }
    float addressedU = 0.0f;
    float addressedV = 0.0f;
    if (!addressUv(u, binding.wrapU, addressedU)
        || !addressUv(v, binding.wrapV, addressedV)) {
        // Decal means the texture contributes nothing outside its domain;
        // scalar/base opacity therefore remains fully present there.
        ++coverage.sampleCount;
        ++coverage.opaqueCount;
        return;
    }
    const int x = std::min(
        image.width - 1, static_cast<int>(addressedU * image.width));
    const int y = std::min(
        image.height - 1, static_cast<int>(addressedV * image.height));
    const std::size_t offset =
        (static_cast<std::size_t>(y) * image.width + x) * 4;
    const unsigned char alpha = image.rgba[offset + (useRedChannel ? 0 : 3)];
    ++coverage.sampleCount;
    if (alpha <= 8) {
        ++coverage.transparentCount;
    } else if (alpha >= 247) {
        ++coverage.opaqueCount;
    } else {
        ++coverage.partialCount;
    }
}

void sampleSubmeshAlpha(MaterialAlphaCoverage& coverage,
                        const MeshData& mesh, const SubmeshData& submesh,
                        const DecodedImage& image, bool useRedChannel,
                        const MaterialData::TextureSource& binding)
{
    if (mesh.uvs.size() < 2 || submesh.startIndex >= mesh.indices.size()) {
        return;
    }
    const std::size_t end = std::min<std::size_t>(
        mesh.indices.size(),
        static_cast<std::size_t>(submesh.startIndex) + submesh.indexCount);
    // Vertices, edge midpoints and centroid cover both thin cutout borders
    // and broad translucent regions without rasterizing full-resolution maps.
    constexpr float barycentric[][3] = {
        {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
        {0.5f, 0.5f, 0.0f}, {0.0f, 0.5f, 0.5f}, {0.5f, 0.0f, 0.5f},
        {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
    };
    for (std::size_t i = submesh.startIndex; i + 2 < end; i += 3) {
        const std::uint32_t i0 = mesh.indices[i + 0];
        const std::uint32_t i1 = mesh.indices[i + 1];
        const std::uint32_t i2 = mesh.indices[i + 2];
        if ((static_cast<std::size_t>(std::max({i0, i1, i2})) * 2 + 1)
            >= mesh.uvs.size()) {
            continue;
        }
        const float u0 = mesh.uvs[i0 * 2 + 0];
        const float v0 = mesh.uvs[i0 * 2 + 1];
        const float u1 = mesh.uvs[i1 * 2 + 0];
        const float v1 = mesh.uvs[i1 * 2 + 1];
        const float u2 = mesh.uvs[i2 * 2 + 0];
        const float v2 = mesh.uvs[i2 * 2 + 1];
        for (const auto& b : barycentric) {
            sampleAlpha(coverage, image,
                u0 * b[0] + u1 * b[1] + u2 * b[2],
                v0 * b[0] + v1 * b[1] + v2 * b[2], useRedChannel,
                binding);
        }
    }
}

void inferMaterialSurfaceModes(IntermediateAsset& asset,
                               const std::string& sourceDirectory)
{
    std::unordered_map<std::string, std::shared_ptr<const DecodedImage>> cache;
    for (std::size_t materialIndex = 0;
         materialIndex < asset.materials.size(); ++materialIndex) {
        MaterialData& material = asset.materials[materialIndex];
        // A scalar opacity/blend declaration is authoritative and must never
        // be downgraded by a mostly opaque texture sample.
        if (material.alphaMode == MaterialAlphaMode::Blend
            && material.surfaceSource == MaterialSurfaceSource::ExplicitSource) {
            continue;
        }

        const MaterialData::TextureSource* base =
            findTextureSource(material, "baseColorTexture");
        const MaterialData::TextureSource* opacity =
            findTextureSource(material, "opacityTexture");
        const bool hasDedicatedOpacity = opacity
            && (!base || !sameMaterialTextureSource(
                base->sourcePath, opacity->sourcePath));
        const MaterialData::TextureSource* evidence =
            hasDedicatedOpacity ? opacity : base;
        if (!evidence) {
            continue;
        }

        // Intermediate meshes currently retain UV0 only. Preserve an
        // authored dedicated opacity map conservatively when the source asks
        // for another channel or generated mapping; guessing with UV0 would
        // be less accurate than deferring the exact sampling to runtime.
        if (evidence->uvChannel != 0
            || evidence->mapping != MaterialTextureMapping::Uv) {
            if (hasDedicatedOpacity) {
                material.alphaMode = MaterialAlphaMode::Blend;
                material.surfaceSource =
                    MaterialSurfaceSource::TextureCoverage;
            }
            continue;
        }

        const std::string imagePath = resolveTextureSourcePath(
            evidence->sourcePath, sourceDirectory);
        const auto image = decodeImageCached(imagePath, cache);
        if (!image) {
            ayt::log::warn(
                "[FBXConverter] alpha coverage skipped material[%zu] '%s': "
                "cannot decode '%s'",
                materialIndex, material.name.c_str(), imagePath.c_str());
            // A distinct authored opacity binding is stronger evidence than
            // an unavailable decoder/source file. Preserve transparency
            // rather than classifying Opaque and deleting the binding later.
            if (hasDedicatedOpacity) {
                material.alphaMode = MaterialAlphaMode::Blend;
                material.surfaceSource =
                    MaterialSurfaceSource::TextureCoverage;
            }
            continue;
        }

        MaterialAlphaCoverage coverage;
        for (const MeshData& mesh : asset.meshes) {
            for (const SubmeshData& submesh : mesh.submeshes) {
                if (submesh.sourceMaterialIndex != materialIndex) continue;
                sampleSubmeshAlpha(coverage, mesh, submesh, *image,
                                   hasDedicatedOpacity, *evidence);
            }
        }
        if (coverage.sampleCount == 0) {
            if (hasDedicatedOpacity) {
                material.alphaMode = MaterialAlphaMode::Blend;
                material.surfaceSource =
                    MaterialSurfaceSource::TextureCoverage;
            }
            continue;
        }

        material.alphaMode = classifyMaterialAlphaCoverage(
            coverage,
            hasDedicatedOpacity ? MaterialAlphaEvidence::DedicatedOpacity
                                : MaterialAlphaEvidence::BaseColorAlpha);
        material.alphaCutoff = inferMaterialAlphaCutoff(
            coverage, material.alphaMode);
        material.surfaceSource = MaterialSurfaceSource::TextureCoverage;
        ayt::log::info(
            "[FBXConverter] alpha coverage material[%zu] '%s': samples=%zu "
            "zero=%zu partial=%zu opaque=%zu -> %s cutoff=%.4f (%s channel)",
            materialIndex, material.name.c_str(), coverage.sampleCount,
            coverage.transparentCount, coverage.partialCount,
            coverage.opaqueCount, alphaModeName(material.alphaMode),
            material.alphaCutoff,
            hasDedicatedOpacity ? "opacity-red" : "base-alpha/coverage");
    }
}

std::unordered_set<size_t> parseMaterialIndices(const std::string& csv)
{
    std::unordered_set<size_t> out;
    const char* cursor = csv.c_str();
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ';' || *cursor == ' '
               || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0') break;
        char* end = nullptr;
        const unsigned long value = std::strtoul(cursor, &end, 10);
        if (end == cursor) {
            while (*cursor != '\0' && *cursor != ',' && *cursor != ';') ++cursor;
            continue;
        }
        out.insert(static_cast<size_t>(value));
        cursor = end;
    }
    return out;
}

std::unordered_set<std::string> parseMaterialNames(const std::string& list)
{
    std::unordered_set<std::string> out;
    size_t begin = 0;
    while (begin <= list.size()) {
        const size_t end = list.find(';', begin);
        std::string name = list.substr(begin, end == std::string::npos
            ? std::string::npos : end - begin);
        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
        name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
        if (!name.empty()) out.insert(std::move(name));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return out;
}

const char* alphaModeName(MaterialAlphaMode mode)
{
    switch (mode) {
    case MaterialAlphaMode::Mask: return "mask";
    case MaterialAlphaMode::Blend: return "blend";
    default: return "opaque";
    }
}

const char* surfaceSourceName(MaterialSurfaceSource source)
{
    switch (source) {
    case MaterialSurfaceSource::ExplicitSource: return "explicit-fbx";
    case MaterialSurfaceSource::TextureCoverage: return "texture-coverage";
    case MaterialSurfaceSource::ConfigOverride: return "config-override";
    default: return "default";
    }
}

void applyMaterialPolicy(std::vector<MaterialData>& materials,
                         const MaterialImportPolicy& policy)
{
    const auto opaque = parseMaterialIndices(policy.opaqueIndices);
    const auto mask = parseMaterialIndices(policy.maskIndices);
    const auto blend = parseMaterialIndices(policy.blendIndices);
    const auto doubleSided = parseMaterialIndices(policy.doubleSidedIndices);
    const auto opaqueNames = parseMaterialNames(policy.opaqueNames);
    const auto maskNames = parseMaterialNames(policy.maskNames);
    const auto blendNames = parseMaterialNames(policy.blendNames);
    const auto doubleSidedNames = parseMaterialNames(policy.doubleSidedNames);

    for (size_t i = 0; i < materials.size(); ++i) {
        int mode = -1;
        if (opaque.count(i) != 0) mode = 0;
        if (mask.count(i) != 0) mode = 1;
        if (blend.count(i) != 0) mode = 2;
        if (opaqueNames.count(materials[i].name) != 0) mode = 0;
        if (maskNames.count(materials[i].name) != 0) mode = 1;
        if (blendNames.count(materials[i].name) != 0) mode = 2;
        if (mode >= 0) {
            materials[i].alphaMode = static_cast<MaterialAlphaMode>(mode);
            materials[i].surfaceSource = MaterialSurfaceSource::ConfigOverride;
        }
        if (doubleSided.count(i) != 0
            || doubleSidedNames.count(materials[i].name) != 0) {
            materials[i].doubleSided = true;
            materials[i].surfaceSource = MaterialSurfaceSource::ConfigOverride;
        }

        // Assimp/FBX commonly exposes the diffuse image again through the
        // TransparencyFactor slot.  It is not an independent opacity map:
        // multiplying its red channel into the diffuse alpha makes blue/cyan
        // regions disappear.  Opaque surfaces never consume opacity maps;
        // Mask/Blend retain one only when it references a genuinely distinct
        // texture.  Their base texture alpha remains authoritative otherwise.
        auto& parameters = materials[i].parameters;
        for (Param& param : parameters) {
            if (param.name == "normalYSign"
                && param.type == MaterialParamType::Float) {
                param.floatValue = policy.normalMapYSign < 0.0f ? -1.0f : 1.0f;
            }
        }
        auto& textureSources = materials[i].textureSources;
        std::string baseColorSource;
        for (const MaterialData::TextureSource& source : textureSources) {
            if (source.parameterName == "baseColorTexture") {
                baseColorSource = source.sourcePath;
                break;
            }
        }
        const bool removeAllOpacity =
            materials[i].alphaMode == MaterialAlphaMode::Opaque;
        const bool removeAliasedOpacity = std::any_of(
            textureSources.begin(), textureSources.end(),
            [&baseColorSource](const MaterialData::TextureSource& source) {
                return source.parameterName == "opacityTexture"
                    && sameMaterialTextureSource(baseColorSource,
                                                 source.sourcePath);
            });
        parameters.erase(
            std::remove_if(parameters.begin(), parameters.end(),
                [&](const Param& param) {
                    if (param.type != MaterialParamType::Texture2D
                        || param.name != "opacityTexture") {
                        return false;
                    }
                    return removeAllOpacity || removeAliasedOpacity;
                }),
            parameters.end());

        textureSources.erase(
            std::remove_if(textureSources.begin(), textureSources.end(),
                [&](const MaterialData::TextureSource& source) {
                    if (source.parameterName != "opacityTexture") {
                        return false;
                    }
                    return removeAllOpacity
                        || sameMaterialTextureSource(baseColorSource,
                                                     source.sourcePath);
                }),
            textureSources.end());

        // Keep the legacy source list coherent for diagnostics and for old
        // consumers that do not understand semantic TextureSource records.
        materials[i].texturePaths.clear();
        for (const MaterialData::TextureSource& source : textureSources) {
            if (std::find(materials[i].texturePaths.begin(),
                          materials[i].texturePaths.end(), source.sourcePath)
                == materials[i].texturePaths.end()) {
                materials[i].texturePaths.push_back(source.sourcePath);
            }
        }

        const bool hasDedicatedOpacity = std::any_of(
            parameters.begin(), parameters.end(),
            [](const Param& param) {
                return param.type == MaterialParamType::Texture2D
                    && param.name == "opacityTexture";
            });
        auto opacitySource = std::find_if(
            parameters.begin(), parameters.end(),
            [](const Param& param) {
                return param.type == MaterialParamType::Float
                    && param.name == "opacitySource";
            });
        const float opacitySourceValue = materialOpacitySourceValue(
            hasDedicatedOpacity ? MaterialOpacitySource::TextureRed
                                : MaterialOpacitySource::BaseColorAlpha);
        if (opacitySource != parameters.end()) {
            opacitySource->floatValue = opacitySourceValue;
        } else {
            Param param;
            param.name = "opacitySource";
            param.type = MaterialParamType::Float;
            param.floatValue = opacitySourceValue;
            parameters.push_back(std::move(param));
        }
    }
}

} // namespace

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
    // Dev raw-reference mode: .aymat points at raw png/jpg/... and the
    // texture converter copies sources verbatim instead of BC7-cooking.
    parser.setPreserveSourceExtension(!_cookTextures);
    parser.setSourceCoordinatePolicy(_sourceCoordinates);
    textureConverter.setRawCopy(!_cookTextures);

    if (!parser.parse(sourcePath)) {
        ayt::log::error("[FBXConverter] Failed to parse FBX: %s", sourcePath.c_str());
        return result;
    }

    auto asset = parser.getResult();
    if (!asset) {
        ayt::log::error("[FBXConverter] Parser returned null asset: %s", sourcePath.c_str());
        return result;
    }

    std::string fbxDir;
    const size_t lastSlash = sourcePath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        fbxDir = sourcePath.substr(0, lastSlash);
    }

    // Automatic evidence is the default; explicit project/import overrides
    // are applied last so users can correct ambiguous authoring data.
    inferMaterialSurfaceModes(*asset, fbxDir);
    applyMaterialPolicy(asset->materials, _materialPolicy);

    for (size_t i = 0; i < asset->materials.size(); ++i) {
        const MaterialData& material = asset->materials[i];
        ayt::log::info(
            "[FBXConverter] material[%zu] name='%s' alpha=%s cutoff=%.3f "
            "doubleSided=%s source=%s",
            i, material.name.c_str(), alphaModeName(material.alphaMode),
            material.alphaCutoff, material.doubleSided ? "true" : "false",
            surfaceSourceName(material.surfaceSource));
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
        // Deduplicate by semantic output path, not source stem.  A single
        // image referenced as base color and opacity has two different GPU
        // sampling contracts and therefore two independently named assets.
        std::set<std::string> processedTextures;

        for (const auto& mat : asset->materials) {
            for (const auto& source : mat.textureSources) {
                if (!processedTextures.insert(source.virtualPath).second) {
                    continue;
                }
                const std::string texName =
                    makeTextureStemFromSourcePath(source.sourcePath);
                auto texResult = textureConverter.convertFromPath(
                    source.sourcePath, texName, fbxDir, source.usageSuffix);
                for (auto& res : texResult.resources) {
                    result.resources.push_back(res);
                }
            }
            if (!mat.textureSources.empty()) {
                continue;
            }
            for (const auto& texPath : mat.texturePaths) {
                const std::string texName = makeTextureStemFromSourcePath(texPath);

                // 去重：同一纹理被多个材质引用时只处理一次
                const std::string legacyVirtual = makeTextureVirtualPathFromSource(
                    texPath, textureConverter.getUsageSuffix(),
                    _cookTextures ? ".aytex" : textureDevExtensionOf(texPath).c_str());
                if (processedTextures.find(legacyVirtual) != processedTextures.end()) {
                    continue;
                }
                processedTextures.insert(legacyVirtual);

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
        if (!mat.textureSources.empty()) {
            continue;
        }
        for (const auto& texPath : mat.texturePaths) {
            // Dev mode: same extension spelling as FBXParser's param ref
            // (textureDevExtensionOf), so this fallback dedups against the
            // emitted param path instead of leaking a dead .aytex edge.
            const char* ext = _cookTextures ? ".aytex" : nullptr;
            const std::string cooked = makeTextureVirtualPathFromSource(
                texPath, textureConverter.getUsageSuffix(),
                ext != nullptr ? ext : textureDevExtensionOf(texPath).c_str());
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

    result.textureMode = _cookTextures ? "cook" : "raw";
    result.materialPolicyTag = _materialPolicy.tag;
    // Keep this sidecar discriminator coupled to the complete cooked FBX v7
    // coordinate, skinning, material-slot and opacity-alias contract.
    result.importerContractTag = kFbxImporterContractTag;
    result.sourceCoordinateTag =
        sourceCoordinatePolicyCacheTag(_sourceCoordinates);

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
