#include "AYResource/ImportJob.h"
#include "ImportOutputTransaction.h"
#include "ImportCacheMetadata.h"
#include "ImportImage.h"
#include <AYResource/Loader/TextureLoader.h>
#include <AYResource/assetsImpl/Texture.h>
#include <AYAssetFormat/AssetFormat.h>

#include <AYIO/File.h>
#include <AYResource/LooseDependency.h>
#include <AYResource/Converter/AudioContentConverter.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#if defined(AY_RESOURCE_HAS_PARTICLE)
#include "AYResource/Converter/ParticleConverter.h"
#endif
#if defined(AY_RESOURCE_HAS_STATS)
#include "AYResource/Converter/StatsConverter.h"
#endif
#if defined(AY_RESOURCE_HAS_ROOMS)
#include <AY2DEditor/RoomAssemblyConverter2D.h>
#endif
#if defined(AY_RESOURCE_HAS_SEQUENCE)
#include <AYSequence/SequenceConverter.h>
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
#include "AYResource/Converter/DeterministicLevelConverter2D.h"
#endif

namespace ayt::resource
{

namespace {

bool fileExists(const std::string& p)
{
    try {
        std::error_code error;
        return !p.empty() && std::filesystem::is_regular_file(detail::importNativePath(std::filesystem::u8path(p)), error) && !error;
    } catch (...) { return false; }
}

std::string stemOf(const std::string& path)
{
    std::string base = path;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base = base.substr(0, dot);
    }
    return base;
}

std::string joinDirFile(const std::string& dir, const std::string& file)
{
    if (dir.empty()) {
        return file;
    }
    const char last = dir.back();
    if (last == '/' || last == '\\') {
        return dir + file;
    }
    return dir + "/" + file;
}

std::string toLowerCopy(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool isSceneExtension(const std::string& ext)
{
    return ext == "fbx" || ext == "gltf" || ext == "glb";
}

void report(const ImportProgressFn& progress, ImportStage stage, float frac, const std::string& msg)
{
    if (!progress) {
        return;
    }
    ImportProgress p;
    p.stage = stage;
    p.fraction = frac;
    p.message = msg;
    progress(p);
}

bool cancelled(ImportCancelToken* cancel)
{
    return cancel != nullptr && cancel->isCancelled();
}

void writeResourceDependencySidecars(const ConversionResult& result, const std::string& root)
{
    std::unordered_map<std::string, std::vector<ConversionResult::Dependency>> edges;
    for (const auto& dependency : result.dependencies) edges[dependency.from].push_back(dependency);
    for (const auto& resource : result.resources) {
        const auto found = edges.find(resource.path);
        if (found == edges.end() || found->second.empty()) continue;
        ConversionResult sidecar;
        sidecar.resources.push_back(resource);
        sidecar.dependencies = found->second;
        const auto text = sidecar.toJson();
        const auto path = joinDirFile(root, looseDependencySidecarPath(resource.path));
        if (!ayt::io::File::atomicWriteUtf8(path, text.data(), text.size()))
            throw std::runtime_error("could not write imported resource dependencies: " + resource.path);
    }
}

bool tryLoadCachedConversion(const ImportOptions& options,
                             ConversionResult& out,
                             std::string& depPathOut)
{
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
    // Level imports revalidate complete source and root identity; ProjectBuild owns the SHA cache.
    if(asset_format::matchesPath(options.sourcePath,asset_format::Id::DeterministicLevelSource))return false;
#endif
#if defined(AY_RESOURCE_HAS_SEQUENCE)
    // Sequence output also depends on referenced scene/animation/audio bytes.
    if(asset_format::matchesPath(options.sourcePath,asset_format::Id::SceneSequence))return false;
#endif
    const std::string baseName = options.outputNamespace.empty()
        ? (options.outputBaseName.empty() ? stemOf(options.sourcePath) : options.outputBaseName) : options.outputNamespace;
    if (baseName.empty()) {
        return false;
    }
    std::string depPath = joinDirFile(options.outputDir,
        ayt::asset_format::appendSuffix(baseName, ayt::asset_format::Id::ImportDependency));
    depPathOut = depPath;
    if (!fileExists(depPath)) {
        depPath = joinDirFile(options.outputDir, baseName + ".aydep.json");
        if (!fileExists(depPath)) return false;
        depPathOut = depPath;
    }

    const auto metadata = detail::readImportJson(depPath);
    const uint64_t srcMtime = ayt::io::File::lastModifiedTime(options.sourcePath);
    const uint64_t depMtime = ayt::io::File::lastModifiedTime(depPath);
    if ((!metadata.is_object() || !metadata.contains("import"))
        && srcMtime != 0 && depMtime != 0 && depMtime < srcMtime) {
        return false;
    }

    if (!metadata.is_object()) {
        return false;
    }

    out = ConversionResult::fromJson(metadata.dump());
    if (!detail::matchesImportMetadata(options, metadata)) return false;
    if (out.resources.empty()) {
        return false;
    }

    // Dev/raw vs release/cook texture mode: a recorded mode that differs
    // from the requested one invalidates the cache deterministically.
    // Sidecars without the field (legacy) are accepted under both modes —
    // old cooked .aytex caches keep working in dev.
    if (!out.textureMode.empty()) {
        const std::string wantMode = options.cookTextures ? "cook" : "raw";
        if (out.textureMode != wantMode) {
            return false;
        }
    }
    if (!options.materialPolicy.tag.empty()
        && out.materialPolicyTag != options.materialPolicy.tag) {
        return false;
    }
    const bool explicitCoordinatePolicy =
        options.sourceCoordinates.mode == SourceCoordinateMode::Manual
        || !options.sourceCoordinates.tag.empty();
    if (explicitCoordinatePolicy
        && out.sourceCoordinateTag
            != sourceCoordinatePolicyCacheTag(options.sourceCoordinates)) {
        return false;
    }

    const std::string ext = importExtensionOf(options.sourcePath);
    // The contract symbol has one out-of-line definition so all cache readers
    // and writers observe the same version even if a stale MSVC object missed
    // a header-only version bump.
    if (isSceneExtension(ext) && out.importerContractTag != kFbxImporterContractTag) {
        std::fprintf(stderr,
                     "[ImportCache] miss: FBX importer contract cached='%s' required='%s'\n",
                     out.importerContractTag.c_str(), kFbxImporterContractTag);
        return false;
    }
    if (options.requireCharacterAssets && isSceneExtension(ext)) {
        bool hasMesh = false;
        bool hasSkel = false;
        for (const auto& res : out.resources) {
            const std::string t = toLowerCopy(res.type);
            if (t == "mesh" && !res.path.empty()) {
                hasMesh = true;
            }
            if (t == "skeleton" && !res.path.empty()) {
                hasSkel = true;
            }
        }
        if (!hasMesh || !hasSkel) {
            return false;
        }
    }
    if (options.requireAnimationAssets && isSceneExtension(ext)) {
        bool hasAnimation = false;
        for (const auto& res : out.resources) {
            if (toLowerCopy(res.type) == "animation" && !res.path.empty()) {
                hasAnimation = true;
                break;
            }
        }
        if (!hasAnimation) {
            return false;
        }
    }

    // Ensure every listed resource file still exists on disk.
    for (const auto& res : out.resources) {
        if (res.path.empty()) {
            continue;
        }
        const std::string abs = joinDirFile(options.outputDir, res.path);
        if (!fileExists(abs)) {
            return false;
        }
    }

    return true;
}

} // namespace

std::string importExtensionOf(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return "";
    }
    return toLowerCopy(path.substr(dot + 1));
}

bool isImportSupportedExtension(const std::string& sourcePath)
{
    if(ayt::asset_format::matchesPath(sourcePath,ayt::asset_format::Id::AudioChartSource)||ayt::asset_format::matchesPath(sourcePath,ayt::asset_format::Id::AudioPerformanceSource))return true;
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
    if(ayt::asset_format::matchesPath(sourcePath, ayt::asset_format::Id::DeterministicLevelSource)) return true;
#endif
#if defined(AY_RESOURCE_HAS_STATS)
    if(ayt::asset_format::matchesPath(sourcePath, ayt::asset_format::Id::StatsSource)) return true;
#endif
#if defined(AY_RESOURCE_HAS_ROOMS)
    if(ayt::asset_format::matchesPath(sourcePath, ayt::asset_format::Id::RoomAssembly2D)) return true;
#endif
#if defined(AY_RESOURCE_HAS_SEQUENCE)
    if(ayt::asset_format::matchesPath(sourcePath, ayt::asset_format::Id::SceneSequence)) return true;
#endif
    const std::string ext = importExtensionOf(sourcePath);
#if defined(AY_RESOURCE_HAS_PARTICLE)
    if (ayt::asset_format::matchesPath(sourcePath, ayt::asset_format::Id::ParticleEffect)) return true;
#endif
    return ext == "fbx"
        || ext == "gltf" || ext == "glb"
        || ext == "png" || ext == "bmp"
        || ext == "tga" || ext == "dds"
        || ext == "jpg" || ext == "jpeg"
        || ext == "wav" || ext == "mp3" || ext == "ogg";
}

std::string importDependencySidecarPath(const ImportOptions& options)
{
    return joinDirFile(options.outputDir, ayt::asset_format::appendSuffix(
        options.outputNamespace.empty() ? (options.outputBaseName.empty() ? stemOf(options.sourcePath) : options.outputBaseName) : options.outputNamespace,
        ayt::asset_format::Id::ImportDependency));
}

bool readImportRecord(const std::string& sidecarPath, ImportRecord& record, std::string* error)
{
    try {
        const auto value = detail::readImportJson(sidecarPath);
        if (!value.is_object() || !value.contains("import"))
            throw std::runtime_error("sidecar has no fingerprinted import recipe");
        const auto& metadata = value.at("import");
        if (metadata.at("version") != 1 && metadata.at("version") != 2)
            throw std::runtime_error("unsupported import metadata version");
        ImportRecord candidate;
        candidate.sidecarPath = sidecarPath;
        candidate.options.sourcePath = metadata.at("source").get<std::string>();
        candidate.options.sourceId = metadata.value("sourceId", std::string{});
        candidate.options.outputDir = detail::importPathText(std::filesystem::absolute(
            std::filesystem::u8path(sidecarPath)).parent_path());
        auto& options = candidate.options;
        detail::decodeImportSettings(metadata.at("settings"), options);
        for (const auto& entry : metadata.at("inputs"))
            candidate.inputPaths.push_back(entry.at("path").get<std::string>());
        candidate.conversion = ConversionResult::fromJson(value.dump());
        if (candidate.conversion.resources.empty() || candidate.inputPaths.empty()
            || candidate.options.sourcePath.empty()) throw std::runtime_error("incomplete import recipe");
        for (const auto& resource : candidate.conversion.resources)
            if (!ayt::io::path::isPortableRelative(resource.path))
                throw std::runtime_error("invalid import resource path");
        record = std::move(candidate);
        if (error) error->clear();
        return true;
    } catch (const std::exception& failure) {
        if (error) *error = failure.what();
        return false;
    }
}

bool isImportRecordCurrent(const ImportRecord& record)
{
    if (isSceneExtension(importExtensionOf(record.options.sourcePath))
        && record.conversion.importerContractTag != kFbxImporterContractTag) return false;
    return detail::matchesImportMetadata(record.options, detail::readImportJson(record.sidecarPath));
}

std::string serializeImportRecipe(const ImportOptions& options)
{
    const detail::ImportJson value = {{"format", "ay.import-recipe"}, {"version", 1},
        {"source", options.sourcePath}, {"output", options.outputDir},
        {"sourceId", options.sourceId},
        {"settings", detail::importSettings(options)}};
    return value.dump(2) + "\n";
}

bool parseImportRecipe(const std::string& json, ImportOptions& options, std::string* error)
{
    try {
        if (json.size() > 1024 * 1024) throw std::runtime_error("import recipe exceeds 1 MiB");
        const auto value = detail::ImportJson::parse(json);
        if (value.at("format") != "ay.import-recipe" || value.at("version") != 1)
            throw std::runtime_error("unsupported import recipe format/version");
        ImportOptions candidate;
        candidate.sourcePath = value.at("source").get<std::string>();
        candidate.outputDir = value.at("output").get<std::string>();
        candidate.sourceId = value.value("sourceId", std::string{});
        if (candidate.sourcePath.empty() || candidate.outputDir.empty()
            || candidate.sourcePath.find('\0') != std::string::npos
            || candidate.outputDir.find('\0') != std::string::npos
            || candidate.sourceId.find('\0') != std::string::npos || candidate.sourceId.size() > 128)
            throw std::runtime_error("invalid import recipe source/output path");
        detail::decodeImportSettings(value.at("settings"), candidate);
        options = std::move(candidate);
        if (error) error->clear();
        return true;
    } catch (const std::exception& failure) {
        if (error) *error = failure.what();
        return false;
    }
}

ImportResult importAsset(const ImportOptions& options,
                         ImportProgressFn progress,
                         ImportCancelToken* cancel)
{
    ImportResult r;
    report(progress, ImportStage::Validate, 0.0f, "validating");

    if (options.sourcePath.empty() || options.sourcePath.find('\0') != std::string::npos) {
        r.error = "sourcePath is empty or contains NUL";
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    if (!fileExists(options.sourcePath)) {
        r.error = "source file does not exist: " + options.sourcePath;
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    if (options.outputDir.empty() || options.outputDir.find('\0') != std::string::npos) {
        r.error = "destinationDir is empty or contains NUL";
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    if (!detail::validOutputNamespace(options.outputNamespace) || options.sourceId.size() > 128
        || options.sourceId.find('\0') != std::string::npos) {
        r.error = "invalid import source identity/output namespace";
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    try {
        ImportOptions verified;
        detail::decodeImportSettings(detail::importSettings(options), verified);
    } catch (const std::exception& failure) {
        r.error = std::string("invalid import settings: ") + failure.what();
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    if (!isImportSupportedExtension(options.sourcePath)) {
        const std::string ext = importExtensionOf(options.sourcePath);
        r.error =
            "unsupported source extension '" + ext +
            "'. Supported: .fbx .gltf .glb .png .bmp .tga .dds .jpg .jpeg .wav .mp3 .ogg";
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }

    if (cancelled(cancel)) {
        r.cancelled = true;
        r.error = "import cancelled";
        report(progress, ImportStage::Cancelled, 1.0f, r.error);
        return r;
    }

    report(progress, ImportStage::CheckCache, 0.1f, "checking cache");
    const auto sidecarName = ayt::asset_format::appendSuffix(
        options.outputNamespace.empty() ? (options.outputBaseName.empty() ? stemOf(options.sourcePath) : options.outputBaseName) : options.outputNamespace,
        ayt::asset_format::Id::ImportDependency);
    try {
        detail::rejectOtherSource(options, detail::readImportJson(
            joinDirFile(options.outputDir, sidecarName)));
    } catch (const std::exception& error) {
        r.error = error.what();
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    // Particle closure includes external textures; source mtime alone cannot
    // validate it. ProjectBuild owns the content-addressed closure cache.
    if (!options.force && !ayt::asset_format::matchesPath(options.sourcePath, ayt::asset_format::Id::ParticleEffect)
        && !ayt::asset_format::matchesPath(options.sourcePath,ayt::asset_format::Id::AudioChartSource)
        && !ayt::asset_format::matchesPath(options.sourcePath,ayt::asset_format::Id::AudioPerformanceSource)
#if defined(AY_RESOURCE_HAS_STATS)
        && !ayt::asset_format::matchesPath(options.sourcePath, ayt::asset_format::Id::StatsSource)
#endif
    ) {
        ConversionResult cached;
        std::string depPath;
        if (tryLoadCachedConversion(options, cached, depPath)) {
            r.ok = true;
            r.usedCache = true;
            r.conversion = std::move(cached);
            r.depSidecarPath = std::move(depPath);
            report(progress, ImportStage::CacheHit, 1.0f, "cache hit");
            report(progress, ImportStage::Done, 1.0f, "done (cache)");
            return r;
        }
    }

    if (cancelled(cancel)) {
        r.cancelled = true;
        r.error = "import cancelled";
        report(progress, ImportStage::Cancelled, 1.0f, r.error);
        return r;
    }

    report(progress, ImportStage::CreateConverter, 0.2f, "creating converter");
    std::unique_ptr<IConverter> converter;
    try {
        converter = IConverter::create(options.sourcePath);
    } catch (const std::exception& e) {
        r.error = std::string("converter ctor threw: ") + e.what();
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    if (converter == nullptr) {
        r.error = "no IConverter for ." + importExtensionOf(options.sourcePath);
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }

    if (cancelled(cancel)) {
        r.cancelled = true;
        r.error = "import cancelled";
        report(progress, ImportStage::Cancelled, 1.0f, r.error);
        return r;
    }

    report(progress, ImportStage::Convert, 0.35f, "converting");
    std::unique_ptr<detail::ImportOutputTransaction> transaction;
    detail::ImportJson originalSource;
    try {
        originalSource = detail::fingerprintFile(std::filesystem::u8path(options.sourcePath));
        transaction = std::make_unique<detail::ImportOutputTransaction>(options.outputDir);
        if (!converter->setOutputNamespace(options.outputNamespace))
            throw std::runtime_error("this importer does not support per-source output namespaces");
        if (!converter->setOutputBaseName(options.outputBaseName))
            throw std::runtime_error("this importer does not support a stable output basename");
        converter->setOutputDir(transaction->stagingRoot());
        converter->setLoadOption(options.loadOption);
        converter->setCookTextures(options.cookTextures);
        if(auto* audio=dynamic_cast<AudioContentConverter*>(converter.get()))audio->setAssetRoot(options.sourceAssetRoot);
#if defined(AY_RESOURCE_HAS_PARTICLE)
        if(auto* particle=dynamic_cast<ParticleConverter*>(converter.get()))
            particle->setAssetRoot(options.sourceAssetRoot);
#endif
#if defined(AY_RESOURCE_HAS_STATS)
        if(auto* stats=dynamic_cast<StatsConverter*>(converter.get()))
            stats->setAssetRoot(options.sourceAssetRoot);
#endif
#if defined(AY_RESOURCE_HAS_ROOMS)
        if(auto* rooms=dynamic_cast<RoomAssemblyConverter2D*>(converter.get()))
            rooms->setAssetRoot(options.sourceAssetRoot);
#endif
#if defined(AY_RESOURCE_HAS_SEQUENCE)
        if(auto* sequence=dynamic_cast<SequenceConverter*>(converter.get()))
            sequence->setAssetRoot(options.sourceAssetRoot);
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
        if(auto* level=dynamic_cast<DeterministicLevelConverter2D*>(converter.get()))
            level->setAssetRoot(options.sourceAssetRoot);
#endif
        converter->setMaterialImportPolicy(options.materialPolicy);
        converter->setSourceCoordinatePolicy(options.sourceCoordinates);
        r.conversion = converter->convert();
        transaction->validate(r.conversion, isSceneExtension(importExtensionOf(options.sourcePath)));
        // Raw-copy is intentionally a low-level byte operation. A completed
        // import promises readable texture artifacts, so validate here before
        // any published file can be replaced.
        for (auto& resource : r.conversion.resources) {
            if (resource.type != "Texture") continue;
            const auto path=joinDirFile(transaction->stagingRoot(),resource.path);
            const auto ext=importExtensionOf(resource.path);
            if (ext=="png" || ext=="jpg" || ext=="jpeg" || ext=="bmp" || ext=="tga") {
                if (options.overrideTextureSampler || !options.keepTextureMipmaps)
                    throw std::runtime_error("texture sampling overrides require cooked texture output");
                detail::ImportImage decoded;
                if (!detail::readImportImage(path,decoded))
                    throw std::runtime_error("invalid converted image: "+resource.path);
            } else {
                std::vector<UInt8> bytes;
                TextureLoader loader;
                if (!detail::readImportBytes(path,bytes))
                    throw std::runtime_error("invalid converted texture: "+resource.path);
                const auto texture = std::dynamic_pointer_cast<Texture>(loader.loadFromBinary(bytes.data(),bytes.size()));
                if (!texture) throw std::runtime_error("invalid converted texture: "+resource.path);
                if (options.overrideTextureSampler || !options.keepTextureMipmaps) {
                    if (options.overrideTextureSampler) {
                        auto sampler = options.textureSampler;
                        sampler.sRGB = texture->getSampler().sRGB;
                        texture->setSampler(sampler);
                    }
                    if (!options.keepTextureMipmaps && texture->getMipmapCount() > 1) {
                        const auto count = texture->getMipmapSize(0);
                        std::vector<UInt8> base(texture->getMipmapData(0),texture->getMipmapData(0)+count);
                        texture->setImageData(std::move(base)); texture->setMipmapLayout({0},{count});
                    }
                    if (!texture->saveToBinary(bytes)
                        || !ayt::io::File::atomicWriteUtf8(path,bytes.data(),bytes.size()))
                        throw std::runtime_error("could not stage texture sampling settings: "+resource.path);
                    resource.size = bytes.size();
                }
            }
        }
    } catch (const std::exception& e) {
        r.conversion = {};
        r.error = std::string("converter threw: ") + e.what();
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }

    if (cancelled(cancel)) {
        // Convert already finished; still report cancel if requested mid-flight
        // after convert returns (best-effort cooperative semantics).
        r.cancelled = true;
        r.conversion = {};
        r.error = "import cancelled";
        report(progress, ImportStage::Cancelled, 1.0f, r.error);
        return r;
    }

    if (options.requireAnimationAssets && isSceneExtension(
            importExtensionOf(options.sourcePath))) {
        bool hasAnimation = false;
        for (const auto& resource : r.conversion.resources) {
            if (toLowerCopy(resource.type) == "animation"
                && !resource.path.empty()) {
                hasAnimation = true;
                break;
            }
        }
        if (!hasAnimation) {
            r.error = "animation source produced no Animation resource";
            report(progress, ImportStage::Failed, 1.0f, r.error);
            return r;
        }
    }

    try {
        report(progress, ImportStage::Convert, .9f, "validating source dependencies");
        if (cancelled(cancel)) {
            r.cancelled = true;
            r.conversion = {};
            r.error = "import cancelled";
            report(progress, ImportStage::Cancelled, 1.0f, r.error);
            return r;
        }
        if (originalSource != detail::fingerprintFile(std::filesystem::u8path(options.sourcePath)))
            throw std::runtime_error("source changed during import; retry after saving the source");
        if (isSceneExtension(importExtensionOf(options.sourcePath)))
            writeResourceDependencySidecars(r.conversion, transaction->stagingRoot());
        detail::writeImportMetadata(options, r.conversion, transaction->stagingRoot(), sidecarName);
        transaction->publish(sidecarName);
    } catch (const std::exception& error) {
        r.conversion = {};
        r.error = std::string("could not publish import: ") + error.what();
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }

    r.ok = true;
    r.usedCache = false;
    r.depSidecarPath = importDependencySidecarPath(options);
#if defined(AY_RESOURCE_HAS_PARTICLE)
    if (dynamic_cast<ParticleConverter*>(converter.get())) {
        for (const auto& resource : r.conversion.resources) {
            if (resource.type == "ParticleEffect") {
                r.depSidecarPath = looseDependencySidecarPath(joinDirFile(options.outputDir, resource.path));
                break;
            }
        }
    }
#endif
    report(progress, ImportStage::Done, 1.0f,
           "done (converted " + std::to_string(r.conversion.resources.size()) + " resources)");
    return r;
}

ImportBatchResult importAssetBatch(const ImportBatchOptions& options,
                                   ImportProgressFn progress,
                                   ImportCancelToken* cancel)
{
    ImportBatchResult batch;
    const size_t n = options.sourcePaths.size();
    batch.results.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        if (cancelled(cancel)) {
            ImportResult skipped;
            skipped.cancelled = true;
            skipped.error = "import cancelled";
            batch.results.push_back(std::move(skipped));
            ++batch.cancelledCount;
            for (size_t j = i + 1; j < n; ++j) {
                ImportResult rest;
                rest.cancelled = true;
                rest.error = "import cancelled";
                batch.results.push_back(std::move(rest));
                ++batch.cancelledCount;
            }
            break;
        }

        ImportOptions one;
        one.sourcePath = options.sourcePaths[i];
        one.outputDir = options.outputDir;
        one.loadOption = options.loadOption;
        one.force = options.force;
        one.requireCharacterAssets = options.requireCharacterAssets;
        one.requireAnimationAssets = options.requireAnimationAssets;
        one.cookTextures = options.cookTextures;
        one.materialPolicy = options.materialPolicy;
        one.sourceCoordinates = options.sourceCoordinates;
        one.allowLegacyCache = options.allowLegacyCache;
        one.overrideTextureSampler = options.overrideTextureSampler;
        one.textureSampler = options.textureSampler;
        one.keepTextureMipmaps = options.keepTextureMipmaps;

        ImportProgressFn wrapped;
        if (progress) {
            wrapped = [&](const ImportProgress& p) {
                ImportProgress scaled = p;
                const float base = static_cast<float>(i) / static_cast<float>(n > 0 ? n : 1);
                const float span = 1.0f / static_cast<float>(n > 0 ? n : 1);
                scaled.fraction = base + span * p.fraction;
                scaled.message = "[" + std::to_string(i + 1) + "/" + std::to_string(n) + "] " + p.message;
                progress(scaled);
            };
        }

        ImportResult r = importAsset(one, wrapped, cancel);
        if (r.cancelled) {
            ++batch.cancelledCount;
        } else if (r.ok) {
            ++batch.okCount;
            if (r.usedCache) {
                ++batch.cacheHitCount;
            }
        } else {
            ++batch.failCount;
            if (options.stopOnError) {
                batch.results.push_back(std::move(r));
                for (size_t j = i + 1; j < n; ++j) {
                    ImportResult rest;
                    rest.error = "skipped after previous error";
                    batch.results.push_back(std::move(rest));
                    ++batch.failCount;
                }
                break;
            }
        }
        batch.results.push_back(std::move(r));
    }

    return batch;
}

} // namespace ayt::resource
