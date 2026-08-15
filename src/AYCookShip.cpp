#include "AYResource/CookShip.h"

#include "AYResource/ResourceBootstrap.h"
#include "AYResource/ResourceRegistry.h"
#include "AYResource/LooseDependency.h"
#include "AYResource/IConverter.h"
#include "AYStorage/IStorageDatabase.h"
#include "AYStorage/IPackageWriter.h"

#include <AYIO/File.h>
#include <AYIO/Path.h>

#include <filesystem>
#include <unordered_set>

namespace ayt::resource
{

namespace {

namespace fs = std::filesystem;

std::string toForwardSlashes(std::string p)
{
    for (char& c : p) {
        if (c == '\\') {
            c = '/';
        }
    }
    return p;
}

std::string makeLogicalPath(const fs::path& root, const fs::path& file)
{
    std::error_code ec;
    const fs::path rel = fs::relative(file, root, ec);
    if (ec) {
        return toForwardSlashes(ayt::io::path::normalize(file.string()));
    }
    return toForwardSlashes(rel.generic_string());
}

bool isCookableAsset(const std::string& path)
{
    const std::string type = ResourceRegistry::getTypeFromPath(path);
    return !type.empty();
}

bool pathMatchesLogical(const std::string& logical, const std::string& ref)
{
    const std::string a = toForwardSlashes(ayt::io::path::normalize(logical));
    const std::string b = toForwardSlashes(ayt::io::path::normalize(ref));
    if (a == b) {
        return true;
    }
    if (a.size() >= b.size()) {
        return a.compare(a.size() - b.size(), b.size(), b) == 0;
    }
    return false;
}

std::string resolveLogicalDep(const std::string& ownerLogical, const std::string& depTo)
{
    if (depTo.empty()) {
        return {};
    }
    // Absolute or already root-relative.
    if (depTo.size() >= 2 && depTo[1] == ':') {
        return toForwardSlashes(ayt::io::path::normalize(depTo));
    }
    if (!depTo.empty() && (depTo[0] == '/' || depTo[0] == '\\')) {
        return toForwardSlashes(ayt::io::path::normalize(depTo));
    }
    const std::string ownerDir = ayt::io::path::directory(ownerLogical);
    if (ownerDir.empty() || ownerDir == ".") {
        return toForwardSlashes(ayt::io::path::normalize(depTo));
    }
    return toForwardSlashes(ayt::io::path::normalize(ayt::io::path::join(ownerDir, depTo)));
}

} // namespace

CookShipResult cookShipPackage(const CookShipOptions& options)
{
    CookShipResult result;

    if (options.assetsRoot.empty() || options.outputDir.empty()) {
        result.error = "assetsRoot and outputDir are required";
        return result;
    }

    const fs::path root = fs::absolute(fs::path(options.assetsRoot));
    if (!fs::exists(root) || !fs::is_directory(root)) {
        result.error = "assetsRoot is not a directory: " + options.assetsRoot;
        return result;
    }

    if (!areLoadersInitialized()) {
        initializeLoaders();
    }

    std::error_code ec;
    fs::create_directories(options.outputDir, ec);
    if (ec) {
        result.error = "failed to create outputDir: " + options.outputDir;
        return result;
    }

    result.pakPath = ayt::io::path::join(options.outputDir, options.pakFileName);
    result.dbPath = ayt::io::path::join(options.outputDir, options.dbFileName);

    // Remove previous ship artifacts for a clean cook.
    if (ayt::io::File::exists(result.pakPath)) {
        fs::remove(result.pakPath, ec);
    }
    if (ayt::io::File::exists(result.dbPath)) {
        fs::remove(result.dbPath, ec);
    }

    auto writer = ayt::storage::IPackageWriter::create(result.pakPath);
    if (!writer) {
        result.error = "failed to create package writer: " + result.pakPath;
        return result;
    }
    writer->setCompressionAlgo(options.compression);

    auto db = ayt::storage::IStorageDatabase::create(result.dbPath);
    if (!db || !db->isOpen()) {
        result.error = "failed to create storage database: " + result.dbPath;
        return result;
    }

    struct AssetEntry {
        std::string diskPath;
        std::string logicalPath;
        std::string type;
    };
    std::vector<AssetEntry> assets;

    const auto collect = [&](const fs::directory_entry& entry) {
        if (!entry.is_regular_file()) {
            return;
        }
        const std::string disk = entry.path().string();
        if (!isCookableAsset(disk)) {
            return;
        }
        AssetEntry a;
        a.diskPath = disk;
        // Match ResourceManager::normalizeResourcePath (platform separators).
        a.logicalPath = ayt::io::path::normalize(makeLogicalPath(root, entry.path()));
        a.type = ResourceRegistry::getTypeFromPath(disk);
        if (!a.logicalPath.empty() && !a.type.empty()) {
            assets.push_back(std::move(a));
        }
    };

    if (options.recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
            collect(entry);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            collect(entry);
        }
    }
    if (ec) {
        result.error = "directory walk failed: " + ec.message();
        return result;
    }

    db->beginBatch();
    for (const AssetEntry& asset : assets) {
        if (!writer->addFile(asset.diskPath, asset.logicalPath)) {
            db->rollbackBatch();
            result.error = "failed to add file to pak: " + asset.diskPath;
            return result;
        }

        ayt::storage::ResourceRecord rec;
        rec.path = asset.logicalPath;
        rec.type = asset.type;
        rec.format = ayt::io::path::extension(asset.diskPath);
        rec.size = static_cast<int64_t>(fs::file_size(asset.diskPath, ec));
        if (ec) {
            rec.size = 0;
            ec.clear();
        }
        // Store pak file name; ResourceManager resolves against DB directory.
        rec.inPackage = options.pakFileName;
        if (!db->insertResource(rec)) {
            db->rollbackBatch();
            result.error = "failed to insert resource: " + asset.logicalPath;
            return result;
        }
        ++result.fileCount;
    }

    // Dependencies from per-asset .aydep.json sidecars.
    std::unordered_set<std::string> seenDeps;
    for (const AssetEntry& asset : assets) {
        const std::string sidecar = looseDependencySidecarPath(asset.diskPath);
        if (!ayt::io::File::exists(sidecar)) {
            continue;
        }
        const std::string json = ayt::io::File::readAllText(sidecar);
        if (json.empty()) {
            continue;
        }
        const ConversionResult info = ConversionResult::fromJson(json);
        for (const auto& dep : info.dependencies) {
            if (!pathMatchesLogical(asset.logicalPath, dep.from)) {
                continue;
            }
            const std::string toLogical = resolveLogicalDep(asset.logicalPath, dep.to);
            if (toLogical.empty()) {
                continue;
            }
            const std::string key = asset.logicalPath + "->" + toLogical;
            if (!seenDeps.insert(key).second) {
                continue;
            }
            if (db->addDependency(asset.logicalPath, toLogical)) {
                ++result.dependencyCount;
            }
        }
    }

    if (!db->commitBatch()) {
        result.error = "failed to commit database batch";
        return result;
    }
    if (!writer->flush()) {
        result.error = "failed to flush package: " + result.pakPath;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ayt::resource
