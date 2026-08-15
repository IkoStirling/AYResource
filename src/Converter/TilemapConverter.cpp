#include "AYResource/Converter/TilemapConverter.h"
#include "AYResource/VirtualAssetPath.h"
#include "AYResource/assetsImpl/TilemapAsset.h"
#include "AYIO/File.h"
#include <AYSerializer.h>
#include <AYStorage/Guid.h>
#include <AYLog.h>
#include <cctype>
#include <cstring>

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

void appendBytes(std::vector<UInt8>& out, const void* data, size_t n) {
    const UInt8* p = static_cast<const UInt8*>(data);
    out.insert(out.end(), p, p + n);
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

    if (cols == 0 || rows == 0 || tileWidth == 0 || tileHeight == 0) {
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

    // Guid covers every data-carrying byte: meta + mode + ids + flags +
    // animation table. A missed field here silently breaks cook caching
    // correctness — a changed frame tile id or duration must invalidate
    // cooked output.
    std::vector<UInt8> contentData;
    appendBytes(contentData, &cols, sizeof(cols));
    appendBytes(contentData, &rows, sizeof(rows));
    appendBytes(contentData, &tileWidth, sizeof(tileWidth));
    appendBytes(contentData, &tileHeight, sizeof(tileHeight));
    const UInt8 modeByte = static_cast<UInt8>(packMode);
    appendBytes(contentData, &modeByte, sizeof(modeByte));
    appendBytes(contentData, &defaultTileId, sizeof(defaultTileId));
    const UInt32 idCount = asset.getTileIdCount();
    if (packMode == TilemapPackMode::Narrow16) {
        appendBytes(contentData, asset.getTileIds16(), idCount * sizeof(UInt16));
    } else {
        appendBytes(contentData, asset.getTileIds32(), idCount * sizeof(UInt32));
    }
    for (const TileCollisionFlagEntry& e : flags) {
        appendBytes(contentData, &e.tileId, sizeof(e.tileId));
        appendBytes(contentData, &e.flags, sizeof(e.flags));
    }
    // Animation bytes in the same layout the binary writes them
    // (count + per-entry {sourceTileId, frameCount, frames}).
    const UInt32 animCount = asset.getAnimationCount();
    appendBytes(contentData, &animCount, sizeof(animCount));
    const TileAnimationEntry* entries = asset.getAnimationEntries();
    for (UInt32 i = 0u; i < animCount; ++i) {
        const TileAnimationEntry& e = entries[i];
        appendBytes(contentData, &e.sourceTileId, sizeof(e.sourceTileId));
        appendBytes(contentData, &e.frameCount, sizeof(e.frameCount));
        if (e.frameCount > 0) {
            appendBytes(contentData, e.frames,
                        e.frameCount * sizeof(TileAnimationFrame));
        }
    }
    lastGuid = ayt::storage::Guid::computeFromData(contentData.data(), contentData.size());
    asset.setGuid(lastGuid);

    std::vector<UInt8> binaryData;
    if (!asset.saveToBinary(binaryData)) {
        return result;
    }

    const std::string baseName = name.empty()
        ? tilemapStem(getFileName(sourcePath)) : name;
    const std::string virtualPath = makeTilemapVirtualPath(baseName);

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
    result.resources.push_back(res);

    return result;
}

} // namespace ayt::resource
