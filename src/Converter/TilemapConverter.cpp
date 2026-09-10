#include "AYResource/Converter/TilemapConverter.h"
#include "AYResource/Converter/TextureConverter.h"
#include "AYResource/VirtualAssetPath.h"
#include "AYResource/assetsImpl/TilemapAsset.h"
#include "AYIO/File.h"
#include <AYSerializer.h>
#include <AYStorage/Guid.h>
#include <AYLog.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>

namespace ayt::resource
{

TilemapConverter::TilemapConverter() = default;

TilemapConverter::TilemapConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void TilemapConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void TilemapConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

namespace {

// "solid|oneWay|slopeL" -> bitmask of TileCollisionFlagBits.
UInt32 parseFlagList(const std::string& flags) {
    UInt32 bits = 0u;
    std::string current;
    for (char ch : flags) {
        if (ch == '|') {
            if (current == "solid")      bits |= TileCollisionFlagBits::Solid;
            else if (current == "oneWay") bits |= TileCollisionFlagBits::OneWay;
            else if (current == "slopeL") bits |= TileCollisionFlagBits::Slope_L;
            else if (current == "slopeR") bits |= TileCollisionFlagBits::Slope_R;
            else if (current == "hazard") bits |= TileCollisionFlagBits::Hazard;
            else if (current == "ladder") bits |= TileCollisionFlagBits::Ladder;
            else if (current == "empty")  bits |= TileCollisionFlagBits::Empty;
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (current == "solid")      bits |= TileCollisionFlagBits::Solid;
    else if (current == "oneWay") bits |= TileCollisionFlagBits::OneWay;
    else if (current == "slopeL") bits |= TileCollisionFlagBits::Slope_L;
    else if (current == "slopeR") bits |= TileCollisionFlagBits::Slope_R;
    else if (current == "hazard") bits |= TileCollisionFlagBits::Hazard;
    else if (current == "ladder") bits |= TileCollisionFlagBits::Ladder;
    else if (current == "empty")  bits |= TileCollisionFlagBits::Empty;
    return bits;
}

// "0x1F" / "31" -> raw bitmask; returns false on non-numeric text.
bool parseNumericFlags(const std::string& text, UInt32& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    unsigned long v = 0;
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        v = std::strtoul(begin + 2, &end, 16);
    } else {
        v = std::strtoul(begin, &end, 10);
    }
    if (end == begin || *end != '\0') {
        return false;
    }
    out = static_cast<UInt32>(v);
    return true;
}

std::string getFileName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

// "ground.aytilemap.json" -> "ground" (used when JSON omits "name").
std::string tilemapStem(const std::string& fileName) {
    std::string stem = fileName;
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) {
        stem = stem.substr(0, dot);          // ground.aytilemap
        dot = stem.find_last_of('.');
        if (dot != std::string::npos) {
            stem = stem.substr(0, dot);      // ground
        }
    }
    return stem;
}

std::optional<std::filesystem::path> resolveAtlasSourcePath(
    const std::string& authoredPath, const std::string& tilemapSourcePath,
    const std::string& assetRoot)
{
    const std::filesystem::path authored =
        std::filesystem::u8path(authoredPath);
    std::vector<std::filesystem::path> candidates;
    if (authored.is_absolute()) {
        candidates.push_back(authored);
    } else {
        if (!assetRoot.empty()) {
            candidates.push_back(std::filesystem::u8path(assetRoot) / authored);
            if (authored.begin() != authored.end()
                && authored.begin()->string() == "Assets") {
                candidates.push_back(
                    std::filesystem::u8path(assetRoot).parent_path()
                    / authored);
            }
        }
        candidates.push_back(
            std::filesystem::u8path(tilemapSourcePath).parent_path()
            / authored);
    }
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error) {
            continue;
        }
        const std::filesystem::path absolute =
            std::filesystem::absolute(candidate, error);
        return (error ? candidate : absolute).lexically_normal();
    }
    return std::nullopt;
}

std::optional<std::string> atlasContentSuffix(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    uint64_t hash = 14695981039346656037ull;
    std::array<char, 64u * 1024u> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(index)]);
            hash *= 1099511628211ull;
        }
    }
    if (input.bad()) return std::nullopt;
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}

std::string safeTextureStem(std::string value)
{
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '_' && ch != '-') ch = '_';
    }
    while (!value.empty() && value.back() == '_') value.pop_back();
    return value.empty() ? "tile_atlas" : value;
}

} // namespace

ConversionResult TilemapConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        return result;
    }

    auto serializer = ayt::serializer::createSerializer(ayt::serializer::Format::Json);
    if (!serializer || !serializer->loadFromFile(sourcePath)) {
        return result;
    }

    serializer->beginObject(nullptr);

    std::string name, mode;
    UInt32 cols = 0, rows = 0, defaultTileId = 0;
    UInt32 tileWidth = 0, tileHeight = 0;
    serializer->field("name", name);
    serializer->field("cols", cols);
    serializer->field("rows", rows);
    serializer->field("tileWidth", tileWidth);
    serializer->field("tileHeight", tileHeight);
    serializer->field("defaultTileId", defaultTileId);
    serializer->field("mode", mode);

    const std::string baseName = name.empty()
        ? tilemapStem(getFileName(sourcePath)) : name;
    const std::string virtualPath = makeTilemapVirtualPath(baseName);

    constexpr uint64_t kMaxCookedCells = 16ull * 1024ull * 1024ull;
    const uint64_t cellCount = static_cast<uint64_t>(cols) * rows;
    if (cols == 0 || rows == 0 || tileWidth == 0 || tileHeight == 0
        || tileWidth > (std::numeric_limits<UInt16>::max)()
        || tileHeight > (std::numeric_limits<UInt16>::max)()
        || cellCount > kMaxCookedCells) {
        ayt::log::warn("[TilemapConverter] '%s' geometry exceeds the cook budget",
                       sourcePath.c_str());
        return result;  // invalid geometry — never emit garbage
    }

    TilemapPackMode packMode = TilemapPackMode::Narrow16;
    if (!mode.empty()) {
        if (mode == "wide32") {
            packMode = TilemapPackMode::Wide32;
        } else if (mode != "narrow16") {
            ayt::log::warn("[TilemapConverter] '%s' unknown mode '%s'; "
                           "defaulting to narrow16", sourcePath.c_str(), mode.c_str());
        }
    }

    if (packMode == TilemapPackMode::Narrow16
        && defaultTileId > (std::numeric_limits<UInt16>::max)()) {
        ayt::log::warn("[TilemapConverter] '%s' defaultTileId exceeds narrow16",
                       sourcePath.c_str());
        return result;
    }

    // Collision flags table first (create() takes it as a parameter).
    std::vector<TileCollisionFlagEntry> flags;
    if (serializer->isFieldPending("collisionFlags")) {
        serializer->beginArray("collisionFlags");
        while (serializer->hasMoreArrayElements()) {
            TileCollisionFlagEntry entry{0u, 0u};
            serializer->beginObject(nullptr);
            serializer->field("tileId", entry.tileId);
            // flags: named list ("solid|oneWay") OR numeric bitmask
            // ("0x1F" / "31" / raw number). peekFieldValue renders any
            // scalar as text (the JSON DOM reader reports every scalar
            // as TokenType::Field — there is no String token — so the
            // text path is the single read path).
            std::string flagText;
            if (serializer->isFieldPending("flags")
                && serializer->peekFieldValue("flags", flagText)) {
                const UInt32 named = parseFlagList(flagText);
                if (named != 0u || flagText.find('|') != std::string::npos
                    || flagText == "empty") {
                    entry.flags = named;
                } else {
                    parseNumericFlags(flagText, entry.flags);
                }
            }
            serializer->endObject();
            flags.push_back(entry);
        }
        serializer->endArray();
    }

    TilemapAsset asset;
    asset.create(cols, rows, static_cast<UInt16>(tileWidth),
                 static_cast<UInt16>(tileHeight), packMode, defaultTileId,
                 flags.empty() ? nullptr : flags.data(),
                 static_cast<UInt32>(flags.size()));
    std::vector<ConversionResult::ConvertedResource> atlasResources;
    std::vector<ConversionResult::Dependency> atlasDependencies;
    std::set<std::string> cookedAtlasPaths;

    // Per-cell tiles (row-major flat list; absent/short = defaultTileId).
    if (serializer->isFieldPending("tiles")) {
        serializer->beginArray("tiles");
        UInt32 index = 0;
        while (serializer->hasMoreArrayElements()) {
            UInt32 tileId = 0;
            serializer->field(nullptr, tileId);
            if (!asset.setTile(index, tileId)) {
                ayt::log::warn("[TilemapConverter] '%s' tile[%u] out of range "
                               "or exceeds pack mode; aborting",
                               sourcePath.c_str(), index);
                return result;
            }
            ++index;
        }
        serializer->endArray();
    }

    // v3 layer stack. Layer zero replaces the legacy compatibility list;
    // short lists retain defaultTileId for their remaining cells.
    if (serializer->isFieldPending("layers")) {
        UInt32 layerIndex = 0u;
        serializer->beginArray("layers");
        while (serializer->hasMoreArrayElements()) {
            bool visible = true;
            std::vector<UInt32> tiles;
            serializer->beginObject(nullptr);
            if (serializer->isFieldPending("visible")) {
                serializer->field("visible", visible);
            }
            if (serializer->isFieldPending("tiles")) {
                serializer->beginArray("tiles");
                while (serializer->hasMoreArrayElements()) {
                    UInt32 tileId = 0u;
                    serializer->field(nullptr, tileId);
                    tiles.push_back(tileId);
                    if (tiles.size() > cellCount) {
                        ayt::log::warn("[TilemapConverter] '%s' layer %u exceeds map cells",
                                       sourcePath.c_str(), layerIndex);
                        return result;
                    }
                }
                serializer->endArray();
            }
            serializer->endObject();
            if (!asset.setLayer(layerIndex, visible,
                                tiles.empty() ? nullptr : tiles.data(),
                                static_cast<UInt32>(tiles.size()))) {
                ayt::log::warn("[TilemapConverter] '%s' invalid layer %u",
                               sourcePath.c_str(), layerIndex);
                return result;
            }
            ++layerIndex;
        }
        serializer->endArray();
        if (layerIndex == 0u) {
            ayt::log::warn("[TilemapConverter] '%s' has an empty layer stack",
                           sourcePath.c_str());
            return result;
        }
    }

    // v3 atlas catalogue and exact source rectangles. Only visual entries
    // backed by an imported atlas are cooked; preview-only/editor entries
    // intentionally remain authoring metadata.
    if (serializer->isFieldPending("tileAssets")) {
        serializer->beginObject("tileAssets");
        if (serializer->isFieldPending("atlases")) {
            serializer->beginArray("atlases");
            while (serializer->hasMoreArrayElements()) {
                UInt32 atlasId = 0u, imageWidth = 0u, imageHeight = 0u;
                std::string atlasSourcePath;
                serializer->beginObject(nullptr);
                serializer->field("atlasId", atlasId);
                serializer->field("sourcePath", atlasSourcePath);
                serializer->field("imageWidth", imageWidth);
                serializer->field("imageHeight", imageHeight);
                serializer->endObject();
                const auto resolved = resolveAtlasSourcePath(
                    atlasSourcePath, sourcePath, outputDir);
                if (!resolved) {
                    ayt::log::warn(
                        "[TilemapConverter] '%s' atlas %u source is unavailable: %s",
                        sourcePath.c_str(), atlasId, atlasSourcePath.c_str());
                    return {};
                }
                const auto contentSuffix = atlasContentSuffix(*resolved);
                if (!contentSuffix) return {};
                TextureConverter texture;
                texture.setOutputDir(outputDir);
                texture.setOutputFormat(TextureFormat::RGBA8);
                texture.setGenerateMipmaps(false);
                texture.setPassthrough(false);
                const std::string textureName = safeTextureStem(
                    resolved->stem().string()) + "_" + *contentSuffix;
                const ConversionResult textureResult = texture.convertFromPath(
                    resolved->string(), textureName, {}, "_tilemap");
                const auto cookedTexture = std::find_if(
                    textureResult.resources.begin(), textureResult.resources.end(),
                    [](const ConversionResult::ConvertedResource& resource) {
                        return resource.type == "Texture";
                    });
                if (cookedTexture == textureResult.resources.end()) {
                    ayt::log::warn(
                        "[TilemapConverter] '%s' failed to cook atlas %u: %s",
                        sourcePath.c_str(), atlasId, resolved->string().c_str());
                    return {};
                }
                if (!asset.addAtlasSource(atlasId, cookedTexture->path,
                                          imageWidth, imageHeight)) {
                    ayt::log::warn("[TilemapConverter] '%s' has an invalid atlas %u",
                                   sourcePath.c_str(), atlasId);
                    return {};
                }
                if (cookedAtlasPaths.insert(cookedTexture->path).second) {
                    atlasResources.push_back(*cookedTexture);
                }
                if (std::none_of(
                        atlasDependencies.begin(), atlasDependencies.end(),
                        [&virtualPath, &cookedTexture](
                            const ConversionResult::Dependency& dependency) {
                            return dependency.from == virtualPath
                                && dependency.to == cookedTexture->path;
                        })) {
                    atlasDependencies.push_back(
                        {virtualPath, cookedTexture->path});
                }
            }
            serializer->endArray();
        }
        if (serializer->isFieldPending("entries")) {
            serializer->beginArray("entries");
            while (serializer->hasMoreArrayElements()) {
                TilemapVisualEntry visual{};
                visual.tintRgba = 0xffffffffu;
                bool hasSourceRect = false;
                serializer->beginObject(nullptr);
                serializer->field("tileId", visual.tileId);
                if (serializer->isFieldPending("atlasId")) {
                    serializer->field("atlasId", visual.atlasId);
                }
                if (serializer->isFieldPending("tintRgba")) {
                    serializer->field("tintRgba", visual.tintRgba);
                }
                if (serializer->isFieldPending("sourceRect")) {
                    UInt32* values[] = {&visual.sourceX, &visual.sourceY,
                                       &visual.sourceWidth, &visual.sourceHeight};
                    size_t valueIndex = 0u;
                    serializer->beginArray("sourceRect");
                    while (serializer->hasMoreArrayElements()) {
                        UInt32 value = 0u;
                        serializer->field(nullptr, value);
                        if (valueIndex < 4u) *values[valueIndex] = value;
                        ++valueIndex;
                    }
                    serializer->endArray();
                    hasSourceRect = valueIndex == 4u;
                }
                serializer->endObject();
                if (visual.atlasId != 0u || hasSourceRect) {
                    if (!hasSourceRect || !asset.addTileVisual(visual)) {
                        ayt::log::warn("[TilemapConverter] '%s' has an invalid visual for tile %u",
                                       sourcePath.c_str(), visual.tileId);
                        return result;
                    }
                }
            }
            serializer->endArray();
        }
        serializer->endObject();
    }

    if (serializer->isFieldPending("shadows")) {
        UInt32 shadowColor = 0x00000080u;
        std::vector<UInt8> masks;
        serializer->beginObject("shadows");
        if (serializer->isFieldPending("colorRgba")) {
            serializer->field("colorRgba", shadowColor);
        }
        if (serializer->isFieldPending("masks")) {
            serializer->beginArray("masks");
            while (serializer->hasMoreArrayElements()) {
                UInt8 mask = 0u;
                serializer->field(nullptr, mask);
                masks.push_back(static_cast<UInt8>(mask & 0x0fu));
                if (masks.size() > cellCount) return result;
            }
            serializer->endArray();
        }
        serializer->endObject();
        if (!masks.empty() && masks.size() != cellCount) {
            masks.resize(static_cast<size_t>(cellCount), 0u);
        }
        if (!asset.setShadowData(shadowColor,
                                 masks.empty() ? nullptr : masks.data(),
                                 static_cast<UInt32>(masks.size()))) {
            return result;
        }
    }

    // Animation table (CM-5): sparse per-source-tile-id flipbook.
    // Schema: "animations": [ { "sourceTileId": 2, "frames":
    //   [ {"tileId": 10, "durationMs": 100}, ... ] } ]
    if (serializer->isFieldPending("animations")) {
        serializer->beginArray("animations");
        while (serializer->hasMoreArrayElements()) {
            UInt32 sourceTileId = 0;
            std::vector<TileAnimationFrame> frames;
            serializer->beginObject(nullptr);
            serializer->field("sourceTileId", sourceTileId);
            if (serializer->isFieldPending("frames")) {
                serializer->beginArray("frames");
                while (serializer->hasMoreArrayElements()) {
                    TileAnimationFrame f{0u, 0u};
                    serializer->beginObject(nullptr);
                    serializer->field("tileId", f.frameTileId);
                    serializer->field("durationMs", f.durationMs);
                    serializer->endObject();
                    frames.push_back(f);
                }
                serializer->endArray();
            }
            serializer->endObject();
            if (frames.empty()) {
                ayt::log::warn("[TilemapConverter] '%s' animations[] entry "
                               "sourceTileId=%u has no frames; aborting",
                               sourcePath.c_str(), sourceTileId);
                return result;  // empty entries are never written to disk
            }
            if (!asset.setAnimationEntry(sourceTileId, frames.data(),
                                         static_cast<UInt32>(frames.size()))) {
                return result;
            }
        }
        serializer->endArray();
    }

    serializer->endObject(); // root

    std::vector<UInt8> binaryData;
    if (!asset.saveToBinary(binaryData)) {
        return result;
    }
    // The binary is now the single canonical digest input. This makes every
    // v3 field (including atlas path, layer visibility, tint and shadows)
    // participate in cook-cache invalidation automatically.
    lastGuid = ayt::storage::Guid::computeFromData(
        binaryData.data(), binaryData.size());
    asset.setGuid(lastGuid);

    if (!outputDir.empty()) {
        const std::string fullPath = outputDir + "/" + virtualPath;
        if (!ayt::io::File::atomicWrite(fullPath, binaryData.data(), binaryData.size())) {
            ayt::log::warn("[TilemapConverter] failed to write %s",
                           fullPath.c_str());
            return result;
        }
    }

    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Tilemap";
    res.size = static_cast<uint64_t>(binaryData.size());
    result.dependencies = std::move(atlasDependencies);
    result.resources.push_back(res);
    result.resources.insert(
        result.resources.end(),
        std::make_move_iterator(atlasResources.begin()),
        std::make_move_iterator(atlasResources.end()));

    return result;
}

} // namespace ayt::resource
