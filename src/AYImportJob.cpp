#include "AYImportJob.h"

#include <AYIO/File.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace ayt::resource
{

namespace {

bool fileExists(const std::string& p)
{
    return !p.empty() && ayt::io::File::exists(p);
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

bool tryLoadCachedConversion(const ImportOptions& options,
                             ConversionResult& out,
                             std::string& depPathOut)
{
    const std::string baseName = stemOf(options.sourcePath);
    if (baseName.empty()) {
        return false;
    }
    const std::string depPath = joinDirFile(options.outputDir, baseName + ".aydep.json");
    depPathOut = depPath;
    if (!fileExists(depPath)) {
        return false;
    }

    const uint64_t srcMtime = ayt::io::File::lastModifiedTime(options.sourcePath);
    const uint64_t depMtime = ayt::io::File::lastModifiedTime(depPath);
    if (srcMtime != 0 && depMtime != 0 && depMtime < srcMtime) {
        return false;
    }

    const std::string json = ayt::io::File::readAllText(depPath);
    if (json.empty()) {
        return false;
    }

    out = ConversionResult::fromJson(json);
    if (out.resources.empty()) {
        return false;
    }

    const std::string ext = importExtensionOf(options.sourcePath);
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
    const std::string ext = importExtensionOf(sourcePath);
    return ext == "fbx"
        || ext == "gltf" || ext == "glb"
        || ext == "png" || ext == "bmp"
        || ext == "tga" || ext == "dds"
        || ext == "wav" || ext == "mp3" || ext == "ogg";
}

ImportResult importAsset(const ImportOptions& options,
                         ImportProgressFn progress,
                         ImportCancelToken* cancel)
{
    ImportResult r;
    report(progress, ImportStage::Validate, 0.0f, "validating");

    if (options.sourcePath.empty()) {
        r.error = "sourcePath is empty";
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    if (!fileExists(options.sourcePath)) {
        r.error = "source file does not exist: " + options.sourcePath;
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    if (options.outputDir.empty()) {
        r.error = "destinationDir is empty";
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }
    if (!isImportSupportedExtension(options.sourcePath)) {
        const std::string ext = importExtensionOf(options.sourcePath);
        r.error =
            "unsupported source extension '" + ext +
            "'. Supported: .fbx .gltf .glb .png .bmp .tga .dds .wav .mp3 .ogg";
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
    if (!options.force) {
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
    try {
        converter->setOutputDir(options.outputDir);
        converter->setLoadOption(options.loadOption);
        r.conversion = converter->convert();
    } catch (const std::exception& e) {
        r.error = std::string("converter threw: ") + e.what();
        report(progress, ImportStage::Failed, 1.0f, r.error);
        return r;
    }

    if (cancelled(cancel)) {
        // Convert already finished; still report cancel if requested mid-flight
        // after convert returns (best-effort cooperative semantics).
        r.cancelled = true;
        r.error = "import cancelled";
        report(progress, ImportStage::Cancelled, 1.0f, r.error);
        return r;
    }

    r.ok = true;
    r.usedCache = false;
    r.depSidecarPath = joinDirFile(options.outputDir, stemOf(options.sourcePath) + ".aydep.json");
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
