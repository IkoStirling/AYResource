#include "AYResource/CookShip.h"

#include "AYResource/ResourceBootstrap.h"
#include "AYResource/ResourceRegistry.h"
#include "AYResource/LooseDependency.h"
#include "AYResource/IConverter.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYStorage/IStorageDatabase.h"
#include "AYStorage/IPackageWriter.h"

#include <AYIO/File.h>
#include <AYIO/Path.h>

#include <cctype>
#include <filesystem>
#include <unordered_set>
#if defined(AY_RESOURCE_HAS_PARTICLE)
#include <AYParticle/EffectPackaging.h>
#endif
#if defined(AY_RESOURCE_HAS_STATS)
#include <AYStats/RecipeAsset.h>
#endif
#if defined(AY_RESOURCE_HAS_ROOMS)
#include <AY2DEditor/RoomAssemblyCook2D.h>
#include <fstream>
#include <vector>
#endif
#if defined(AY_RESOURCE_HAS_SEQUENCE)
#include <AYSequence/SequenceCooked.h>
#include <AYSequence/SequenceDependencies.h>
#include <fstream>
#include <vector>
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
#include <AYEntity/DeterministicLevelCooked2D.h>
#include <fstream>
#include <vector>
#endif

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

#if defined(AY_RESOURCE_HAS_DET_LEVEL)
bool validateDetLevelCookedFile(const fs::path& path,std::string& error) {
    std::error_code ec;const auto status=fs::symlink_status(path,ec);
    if(ec || !fs::is_regular_file(status)) {error="Cooked level must be an ordinary file";return false;}
    const auto size=fs::file_size(path,ec);
    if(ec || size<entity::kDetLevelCooked2DHeaderBytes || size>entity::kDetLevelCooked2DMaxBytes) {
        error="Cooked level exceeds the bounded envelope";return false;
    }
    std::ifstream stream(path,std::ios::binary);
    if(!stream) {error="Cannot open cooked level";return false;}
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(stream.gcount()!=static_cast<std::streamsize>(bytes.size()) || stream.peek()!=std::char_traits<char>::eof() || stream.bad()) {
        error="Cooked level changed size or could not be read completely";return false;
    }
    entity::DetLevel2D level;return entity::decodeDetLevelCooked2D(bytes,level,error);
}
#endif

bool isCookableAsset(const std::string& path)
{
    const auto match = ayt::asset_format::matchPath(path);
    if (!match || match->alias || !match->descriptor->looseCookedPackage)
        return false;
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
    // Converter virtual paths are rooted at the Assets/cooked tree. Treating
    // e.g. a Tilemap's `textures/atlas.aytex` edge as sibling-relative would
    // produce `tilemaps/textures/atlas.aytex` in resources.db. Keep this list
    // aligned with AssetPath's cooked virtual-root contract.
    static const char* kVirtualRoots[] = {
        "materials/", "textures/", "meshes/", "skeletons/",
        "animations/", "shaders/", "tilemaps/", "scripts/", "effects/"
    };
    std::string normalized = toForwardSlashes(
        ayt::io::path::normalize(depTo));
    std::string lower = normalized;
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(
            static_cast<unsigned char>(ch)));
    }
    for (const char* root : kVirtualRoots) {
        if (lower.rfind(root, 0u) == 0u) {
            return normalized;
        }
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

#if defined(AY_RESOURCE_HAS_PARTICLE)
    // Reject a broken closure before touching an earlier ship artifact. CookShip
    // consumes final assets only; source PNG and owner-relative paths need import.
    auto validateParticle=[&](const fs::directory_entry& entry) {
        if(!entry.is_regular_file() || ResourceRegistry::getTypeFromPath(entry.path().string())!="ParticleEffect") return true;
        particle::EffectSource effect; std::string error;
        if(!particle::inspectEffectSource(entry.path(),root,effect,&error)) { result.error=error; return false; }
        for(size_t index=0; index<effect.textures.size(); ++index) {
            const auto& texture=effect.textures[index];
            if(texture.empty()) continue;
            if(!ayt::asset_format::isCanonicalPath(texture.string(), ayt::asset_format::Id::Texture)
               || texture.lexically_relative(effect.root).generic_string()!=effect.asset.emitters[index].effect.texturePath) {
                result.error="Cook particle effects and their textures before packing: "+entry.path().string();
                return false;
            }
        }
        return true;
    };
    std::error_code particleScanError;
    if(options.recursive) {
        for(const auto& entry:fs::recursive_directory_iterator(root,particleScanError))
            if(!validateParticle(entry)) return result;
    } else {
        for(const auto& entry:fs::directory_iterator(root,particleScanError))
            if(!validateParticle(entry)) return result;
    }
    if(particleScanError) { result.error="Particle input scan failed: "+particleScanError.message(); return result; }
#endif

#if defined(AY_RESOURCE_HAS_STATS)
    // Validate final, self-contained recipe assets before replacing a prior package.
    auto validateStats=[&](const fs::directory_entry& entry) {
        if(!entry.is_regular_file() || ResourceRegistry::getTypeFromPath(entry.path().string())!="StatsRecipes") return true;
        auto decoded=stats::decodeRecipeAsset(ayt::io::File::readAllText(entry.path().string()));
        if(decoded)return true;
        result.error="Invalid stats recipe asset: "+entry.path().string()+": "+decoded.error().message;
        return false;
    };
    std::error_code statsScanError;
    if(options.recursive) {
        for(const auto& entry:fs::recursive_directory_iterator(root,statsScanError))
            if(!validateStats(entry)) return result;
    } else {
        for(const auto& entry:fs::directory_iterator(root,statsScanError))
            if(!validateStats(entry)) return result;
    }
    if(statsScanError) {result.error="Stats input scan failed: "+statsScanError.message();return result;}
#endif

#if defined(AY_RESOURCE_HAS_ROOMS)
    auto validateRoom=[&](const fs::directory_entry& entry) {
        if (!entry.is_regular_file() || ResourceRegistry::getTypeFromPath(
                entry.path().string())!="RoomAssembly2D") return true;
        std::error_code ec;
        const auto length=entry.file_size(ec);
        if(ec || length>ayt::ay2d::editor::kRoomAssemblyCooked2DMaxBytes) {
            result.error="Invalid cooked room size: "+entry.path().string();return false;
        }
        std::ifstream input(entry.path(),std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if(!input.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()))
            || input.peek()!=std::char_traits<char>::eof() || input.bad()) {
            result.error="Cannot read cooked room: "+entry.path().string();return false;
        }
        ayt::ay2d::RoomAssemblyConfig2D parsed;
        std::string error;
        if(!ayt::ay2d::editor::decodeRoomAssemblyCooked2D(bytes,parsed,error)) {
            result.error="Invalid cooked room: "+entry.path().string()+": "+error;
            return false;
        }
        return true;
    };
    std::error_code roomScanError;
    if(options.recursive) {
        for(const auto& entry:fs::recursive_directory_iterator(root,roomScanError))
            if(!validateRoom(entry))return result;
    } else {
        for(const auto& entry:fs::directory_iterator(root,roomScanError))
            if(!validateRoom(entry))return result;
    }
    if(roomScanError) {result.error="Room input scan failed: "+roomScanError.message();return result;}
#endif

#if defined(AY_RESOURCE_HAS_SEQUENCE)
    auto validateSequence=[&](const fs::directory_entry& entry) {
        if(!entry.is_regular_file() || !ayt::asset_format::matchesPath(
                entry.path().string(),ayt::asset_format::Id::SceneSequenceCooked))return true;
        std::error_code ec;const auto length=entry.file_size(ec);
        if(ec || length>32u+16u*1024u*1024u) {
            result.error="Invalid cooked sequence size: "+entry.path().string();return false;
        }
        std::ifstream input(entry.path(),std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if(!input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()))
           || input.peek()!=std::char_traits<char>::eof() || input.bad()) {
            result.error="Cannot read cooked sequence: "+entry.path().string();return false;
        }
        sequence::Sequence authored;sequence::SequenceCookedMetadata metadata;
        sequence::SequenceDependencyManifest manifest;sequence::Diagnostic error;
        if(!sequence::decodeCookedSequence(bytes,authored,&metadata,&error)
           || !sequence::buildSequenceDependencyManifest(authored,root,manifest,&error)
           || metadata.dependencySignature!=manifest.signature) {
            result.error="Invalid cooked sequence or missing dependency: "
                +entry.path().string()+": "+error.message;return false;
        }
        return true;
    };
    std::error_code sequenceScanError;
    if(options.recursive) {
        for(const auto& entry:fs::recursive_directory_iterator(root,sequenceScanError))
            if(!validateSequence(entry))return result;
    } else {
        for(const auto& entry:fs::directory_iterator(root,sequenceScanError))
            if(!validateSequence(entry))return result;
    }
    if(sequenceScanError) {result.error="Sequence input scan failed: "
        +sequenceScanError.message();return result;}
#endif

#if defined(AY_RESOURCE_HAS_DET_LEVEL)
    // Reject damaged final assets before replacing an existing package/database.
    auto validateLevel=[&](const fs::directory_entry& entry) {
        if(!asset_format::matchesPath(entry.path().string(),asset_format::Id::DeterministicLevelCooked))return true;
        if(ResourceRegistry::getTypeFromPath(entry.path().string())!="DeterministicLevel2D") {
            result.error="Cooked level suffix belongs to a different resource type";return false;
        }
        std::string error;
        if(validateDetLevelCookedFile(entry.path(),error))return true;
        result.error="Invalid cooked deterministic level: "+entry.path().string()+": "+error;return false;
    };
    std::error_code levelScanError;
    if(options.recursive) {
        fs::recursive_directory_iterator it(root,levelScanError),end;
        while(!levelScanError && it!=end) {if(!validateLevel(*it))return result;it.increment(levelScanError);}
    } else {
        fs::directory_iterator it(root,levelScanError),end;
        while(!levelScanError && it!=end) {if(!validateLevel(*it))return result;it.increment(levelScanError);}
    }
    if(levelScanError) {result.error="Level input scan failed: "+levelScanError.message();return result;}
#endif
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
#if defined(AY_RESOURCE_HAS_PARTICLE)
        if(asset.type=="ParticleEffect") {
            particle::EffectAsset effect;
            if(particle::loadEffectAsset(asset.diskPath,effect)) for(const auto& emitter:effect.emitters) {
                const auto& texture=emitter.effect.texturePath;
                if(texture.empty()) continue;
                const auto logical=ayt::io::path::normalize(texture);
                if(seenDeps.insert(asset.logicalPath+"->"+logical).second
                   && db->addDependency(asset.logicalPath,logical)) ++result.dependencyCount;
            }
        }
#endif
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
            const std::string toLogical = ayt::io::path::normalize(resolveLogicalDep(asset.logicalPath, dep.to));
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
