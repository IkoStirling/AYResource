#include "AYResource/assetsImpl/TilemapAsset.h"
#include <AYIO/File.h>
#include <cstring>
#include <cstdint>

namespace ayt::resource
{

// ===== .aytilemap binary format =====
//
// All multi-byte fields are host-endian little-endian (matches the AYPhysics
// / AYAudio precedent; the engine targets little-endian runtimes). The header
// is packed; the payload follows contiguously.
//
//   offset  field           notes
//   0       UInt32 magic    = 0x4D545941 ('AYTM')
//   4       UInt16 version  = 3 (current). 1/2 remain readable.
//   6       UInt16 tileWidth
//   8       UInt16 tileHeight
//  10       UInt32 cols
//  14       UInt32 rows
//  18       UInt8  mode     // 0 = Narrow16, 1 = Wide32
//  19       UInt8  reserved = 0
//  20       UInt32 defaultTileId
//  24       UInt32 collisionFlagsCount   // v2: # of {tileId,flags} entries
//  28       UInt32 tileIdsCount   // = cols * rows
//  32       UInt32 collisionFlags[collisionFlagsCount]  (v2: 8 bytes each:
//             UInt32 tileId + UInt32 flags; omitted if 0)
//  32+8*collisionFlagsCount
//           UInt16 tileIds[tileIdsCount]  (Narrow16)  OR
//           UInt32 tileIds[tileIdsCount]  (Wide32)
//  [end of v2 pre-CM-5 layout: readers of that era ignore any trailing bytes]
//
//  32+8*collisionFlagsCount+tileIdsBytes
//           UInt32 animationEntryCount        // optional segment (CM-5)
//           entry[animationEntryCount]:
//             UInt32 sourceTileId
//             UInt32 frameCount               // 0 rejected by the reader
//             frame[frameCount]: { UInt32 frameTileId; UInt32 durationMs }
//
// v3 follows the animation segment with a fixed extension header, extra
// layer payloads, atlas paths, visual records and semantic shadow masks.
// Layer zero stays in the legacy payload above so old engine concepts retain
// a cheap compatibility view; only layers 1..N are stored in the extension.
//
// v1 back-compat: a v1 file stores `blockedCount` at offset 24 followed by
// `UInt32 blockedTileIds[blockedCount]` (4 bytes each). The reader normalizes
// every blocked id to a v2 entry with flags = TileCollisionFlagBits::Solid.
#pragma pack(push, 1)
struct TilemapBinaryHeader {
    UInt32 magic;
    UInt16 version;
    UInt16 tileWidth;
    UInt16 tileHeight;
    UInt32 cols;
    UInt32 rows;
    UInt8  mode;
    UInt8  reserved;
    UInt32 defaultTileId;
    UInt32 collisionFlagsCount;   // v2 entry count (v1: blockedCount)
    UInt32 tileIdsCount;
};
#pragma pack(pop)
static_assert(sizeof(TilemapBinaryHeader) == 32, "AYTilemap header must be 32 bytes");

constexpr UInt32 kTilemapV3Magic = 0x33564D54u; // 'TMV3'

#pragma pack(push, 1)
struct TilemapV3ExtensionHeader {
    UInt32 magic;
    UInt32 layerCount;
    UInt32 atlasCount;
    UInt32 visualCount;
    UInt32 shadowColorRgba;
    UInt32 shadowMaskCount;
    UInt32 baseLayerVisible;
};

struct TilemapV3LayerHeader {
    UInt32 visible;
    UInt32 tileIdCount;
};

struct TilemapV3AtlasHeader {
    UInt32 atlasId;
    UInt32 imageWidth;
    UInt32 imageHeight;
    UInt32 pathBytes;
};
#pragma pack(pop)

static_assert(sizeof(TilemapV3ExtensionHeader) == 28);
static_assert(sizeof(TilemapV3LayerHeader) == 8);
static_assert(sizeof(TilemapV3AtlasHeader) == 16);
static_assert(sizeof(TilemapVisualEntry) == 28);

// ===== TilemapAsset =====

void TilemapAsset::clear() {
    _tileWidth = 0;
    _tileHeight = 0;
    _cols = 0;
    _rows = 0;
    _mode = TilemapPackMode::Narrow16;
    _defaultTileId = 0;
    _tileIds16.clear();
    _tileIds32.clear();
    _baseLayerVisible = true;
    _extraLayers.clear();
    _atlases.clear();
    _atlasEntriesView.clear();
    _atlasViewDirty = false;
    _visuals.clear();
    _shadowColorRgba = 0x00000080u;
    _shadowMasks.clear();
    _tileCollisionFlags.clear();
    _animations.clear();
    _animFlat.clear();
    _animEntriesView.clear();
    _animViewDirty = false;
    _name.clear();
    _loaded = false;
}

bool TilemapAsset::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t TilemapAsset::sizeInBytes() const {
    size_t tileBytes = (_mode == TilemapPackMode::Narrow16)
        ? _tileIds16.size() * sizeof(UInt16)
        : _tileIds32.size() * sizeof(UInt32);
    // Animation segment on disk: count + per-entry {sourceTileId, frameCount,
    // frames}. Mirrors saveToBinary so the accounting stays honest.
    size_t animBytes = sizeof(UInt32);
    for (const StoredAnimationEntry& e : _animations) {
        animBytes += sizeof(UInt32) * 2
                   + e.frames.size() * sizeof(TileAnimationFrame);
    }
    size_t extraLayerBytes = 0u;
    for (const StoredLayer& layer : _extraLayers) {
        extraLayerBytes += _mode == TilemapPackMode::Narrow16
            ? layer.tileIds16.size() * sizeof(UInt16)
            : layer.tileIds32.size() * sizeof(UInt32);
    }
    size_t atlasBytes = 0u;
    for (const StoredAtlasEntry& atlas : _atlases) {
        atlasBytes += sizeof(TilemapV3AtlasHeader) + atlas.sourcePath.size();
    }
    return sizeof(TilemapAsset) + tileBytes + extraLayerBytes + atlasBytes
         + _tileCollisionFlags.size() * sizeof(TileCollisionFlagEntry)
         + _visuals.size() * sizeof(TilemapVisualEntry)
         + _shadowMasks.size() + animBytes + _name.size();
}

bool TilemapAsset::load(const std::string& path) {
    _path = path;

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    const size_t fileSize = file.size();
    if (fileSize < sizeof(TilemapBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool TilemapAsset::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(TilemapBinaryHeader)) {
        return false;
    }

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const TilemapBinaryHeader* header = reinterpret_cast<const TilemapBinaryHeader*>(ptr);

    if (header->magic != ITilemap::MAGIC) {
        return false;
    }
    // Accept v1/v2 compatibility assets and the current v3 authoring bridge.
    const UInt16 version = header->version;
    if (version != 1u && version != 2u && version != 3u) {
        return false;
    }

    // Validate pack mode.
    const UInt8 modeVal = header->mode;
    if (modeVal > static_cast<UInt8>(TilemapPackMode::Wide32)) {
        return false;
    }
    const TilemapPackMode mode = static_cast<TilemapPackMode>(modeVal);

    // Bounds-check the payload. v1 entries are 4 bytes (bare UInt32 tileId);
    // v2 entries are 8 bytes (UInt32 tileId + UInt32 flags). tileIdsCount *
    // elemSize can be up to (2^32-1)*4 which overflows uint32; compute in uint64.
    const uint64_t flagsEntryBytes = (version == 1u)
        ? static_cast<uint64_t>(header->collisionFlagsCount) * sizeof(UInt32)
        : static_cast<uint64_t>(header->collisionFlagsCount) * sizeof(TileCollisionFlagEntry);
    const uint64_t elemSize = (mode == TilemapPackMode::Narrow16)
        ? static_cast<uint64_t>(sizeof(UInt16))
        : static_cast<uint64_t>(sizeof(UInt32));
    const uint64_t tileIdsBytes =
        static_cast<uint64_t>(header->tileIdsCount) * elemSize;
    const uint64_t headerBytes = sizeof(TilemapBinaryHeader);
    if (flagsEntryBytes > size - headerBytes) return false;
    if (tileIdsBytes > (size - headerBytes) - flagsEntryBytes) return false;

    clear();

    _tileWidth      = header->tileWidth;
    _tileHeight     = header->tileHeight;
    _cols           = header->cols;
    _rows           = header->rows;
    _mode           = mode;
    _defaultTileId  = header->defaultTileId;

    // Per-tile-id collision flags (v2) or legacy blocked-id list (v1 -> Solid).
    size_t offset = headerBytes;
    if (header->collisionFlagsCount > 0) {
        _tileCollisionFlags.resize(header->collisionFlagsCount);
        if (version == 1u) {
            // v1: bare UInt32 blocked ids -> normalize to Solid entries.
            const UInt32* blocked = reinterpret_cast<const UInt32*>(ptr + offset);
            for (UInt32 i = 0u; i < header->collisionFlagsCount; ++i) {
                _tileCollisionFlags[i].tileId = blocked[i];
                _tileCollisionFlags[i].flags  = TileCollisionFlagBits::Solid;
            }
        } else {
            // v2: {tileId, flags} entries, copied verbatim.
            std::memcpy(_tileCollisionFlags.data(), ptr + offset,
                        static_cast<size_t>(flagsEntryBytes));
        }
        offset += static_cast<size_t>(flagsEntryBytes);
    }

    // Tile ids.
    if (mode == TilemapPackMode::Narrow16) {
        _tileIds16.resize(header->tileIdsCount);
        if (header->tileIdsCount > 0) {
            std::memcpy(_tileIds16.data(), ptr + offset,
                        static_cast<size_t>(tileIdsBytes));
        }
    } else {
        _tileIds32.resize(header->tileIdsCount);
        if (header->tileIdsCount > 0) {
            std::memcpy(_tileIds32.data(), ptr + offset,
                        static_cast<size_t>(tileIdsBytes));
        }
    }
    offset += static_cast<size_t>(tileIdsBytes);

    // ===== Optional trailing animation segment (CM-5) =====
    // Files written before the segment existed end exactly at `offset` —
    // load them with an empty table. Any other trailing bytes must parse as
    // a complete segment and consume the file exactly (strict tail
    // consumption acts as a version lock; see the format comment at the top
    // of this file).
    _animations.clear();
    _animViewDirty = true;
    if (offset < size) {
        if (size - offset < sizeof(UInt32)) {
            clear();
            return false;
        }
        const UInt32 animCount = *reinterpret_cast<const UInt32*>(ptr + offset);
        offset += static_cast<size_t>(sizeof(UInt32));
        _animations.reserve(animCount);
        for (UInt32 i = 0u; i < animCount; ++i) {
            if (size - offset < sizeof(UInt32) * 2) {
                clear();
                return false;  // sourceTileId + frameCount
            }
            const UInt32 sourceTileId =
                *reinterpret_cast<const UInt32*>(ptr + offset);
            const UInt32 frameCount =
                *reinterpret_cast<const UInt32*>(ptr + offset + sizeof(UInt32));
            offset += sizeof(UInt32) * 2;
            if (frameCount == 0) {
                clear();
                return false;  // empty entries are never written
            }
            // frameCount * sizeof(TileAnimationFrame) can overflow uint32;
            // compute in uint64 (same pattern as tileIdsBytes above).
            const uint64_t framesBytes =
                static_cast<uint64_t>(frameCount) * sizeof(TileAnimationFrame);
            if (framesBytes > static_cast<uint64_t>(size - offset)) {
                clear();
                return false;
            }
            StoredAnimationEntry e;
            e.sourceTileId = sourceTileId;
            e.frames.resize(frameCount);
            std::memcpy(e.frames.data(), ptr + offset,
                        static_cast<size_t>(framesBytes));
            offset += static_cast<size_t>(framesBytes);
            _animations.push_back(std::move(e));
        }
        if (version <= 2u && offset != size) {
            clear();
            return false;  // strict tail consumption (version lock)
        }
    }

    if (version == 3u) {
        if (size - offset < sizeof(TilemapV3ExtensionHeader)) {
            clear();
            return false;
        }
        TilemapV3ExtensionHeader extension{};
        std::memcpy(&extension, ptr + offset, sizeof(extension));
        offset += sizeof(extension);
        if (extension.magic != kTilemapV3Magic || extension.layerCount == 0u
            || extension.layerCount > 1024u
            || extension.atlasCount > 65536u
            || extension.visualCount > 16u * 1024u * 1024u
            || (extension.shadowMaskCount != 0u
                && extension.shadowMaskCount != header->tileIdsCount)) {
            clear();
            return false;
        }
        _baseLayerVisible = extension.baseLayerVisible != 0u;
        _extraLayers.reserve(extension.layerCount - 1u);
        for (UInt32 layerIndex = 1u; layerIndex < extension.layerCount;
             ++layerIndex) {
            if (size - offset < sizeof(TilemapV3LayerHeader)) {
                clear();
                return false;
            }
            TilemapV3LayerHeader layerHeader{};
            std::memcpy(&layerHeader, ptr + offset, sizeof(layerHeader));
            offset += sizeof(layerHeader);
            if (layerHeader.tileIdCount != header->tileIdsCount) {
                clear();
                return false;
            }
            const uint64_t bytes = static_cast<uint64_t>(
                layerHeader.tileIdCount) * elemSize;
            if (bytes > static_cast<uint64_t>(size - offset)) {
                clear();
                return false;
            }
            StoredLayer layer;
            layer.visible = layerHeader.visible != 0u;
            if (_mode == TilemapPackMode::Narrow16) {
                layer.tileIds16.resize(layerHeader.tileIdCount);
                std::memcpy(layer.tileIds16.data(), ptr + offset,
                            static_cast<size_t>(bytes));
            } else {
                layer.tileIds32.resize(layerHeader.tileIdCount);
                std::memcpy(layer.tileIds32.data(), ptr + offset,
                            static_cast<size_t>(bytes));
            }
            offset += static_cast<size_t>(bytes);
            _extraLayers.push_back(std::move(layer));
        }
        _atlases.reserve(extension.atlasCount);
        for (UInt32 i = 0u; i < extension.atlasCount; ++i) {
            if (size - offset < sizeof(TilemapV3AtlasHeader)) {
                clear();
                return false;
            }
            TilemapV3AtlasHeader atlasHeader{};
            std::memcpy(&atlasHeader, ptr + offset, sizeof(atlasHeader));
            offset += sizeof(atlasHeader);
            if (atlasHeader.pathBytes == 0u
                || atlasHeader.pathBytes > 1024u * 1024u
                || atlasHeader.imageWidth == 0u || atlasHeader.imageHeight == 0u
                || atlasHeader.pathBytes > size - offset) {
                clear();
                return false;
            }
            StoredAtlasEntry atlas;
            atlas.atlasId = atlasHeader.atlasId;
            atlas.imageWidth = atlasHeader.imageWidth;
            atlas.imageHeight = atlasHeader.imageHeight;
            atlas.sourcePath.assign(
                reinterpret_cast<const char*>(ptr + offset),
                atlasHeader.pathBytes);
            offset += atlasHeader.pathBytes;
            _atlases.push_back(std::move(atlas));
        }
        const uint64_t visualBytes = static_cast<uint64_t>(
            extension.visualCount) * sizeof(TilemapVisualEntry);
        if (visualBytes > static_cast<uint64_t>(size - offset)) {
            clear();
            return false;
        }
        _visuals.resize(extension.visualCount);
        if (visualBytes > 0u) {
            std::memcpy(_visuals.data(), ptr + offset,
                        static_cast<size_t>(visualBytes));
            offset += static_cast<size_t>(visualBytes);
        }
        _shadowColorRgba = extension.shadowColorRgba;
        if (extension.shadowMaskCount > size - offset) {
            clear();
            return false;
        }
        _shadowMasks.assign(ptr + offset,
                            ptr + offset + extension.shadowMaskCount);
        offset += extension.shadowMaskCount;
        _atlasViewDirty = true;
        if (offset != size) {
            clear();
            return false;
        }
    }

    _loaded = true;
    return true;
}

bool TilemapAsset::saveToBinary(std::vector<UInt8>& outData) const {
    const UInt32 tileIdsCount = getTileIdCount();
    const uint64_t flagsBytes =
        static_cast<uint64_t>(_tileCollisionFlags.size()) * sizeof(TileCollisionFlagEntry);
    const uint64_t elemSize = (_mode == TilemapPackMode::Narrow16)
        ? static_cast<uint64_t>(sizeof(UInt16))
        : static_cast<uint64_t>(sizeof(UInt32));
    const uint64_t tileIdsBytes =
        static_cast<uint64_t>(tileIdsCount) * elemSize;
    // Animation segment: always written (count may be 0) so the reader's
    // strict tail consumption never trips on our own files. Mirrors the
    // layout documented at the top of this file.
    uint64_t animBytes = sizeof(UInt32);
    for (const StoredAnimationEntry& e : _animations) {
        animBytes += sizeof(UInt32) * 2
                   + static_cast<uint64_t>(e.frames.size()) * sizeof(TileAnimationFrame);
    }
    uint64_t extensionBytes = sizeof(TilemapV3ExtensionHeader);
    for (const StoredLayer& layer : _extraLayers) {
        const uint64_t layerCount = _mode == TilemapPackMode::Narrow16
            ? layer.tileIds16.size() : layer.tileIds32.size();
        extensionBytes += sizeof(TilemapV3LayerHeader) + layerCount * elemSize;
    }
    for (const StoredAtlasEntry& atlas : _atlases) {
        extensionBytes += sizeof(TilemapV3AtlasHeader) + atlas.sourcePath.size();
    }
    extensionBytes += static_cast<uint64_t>(_visuals.size())
                    * sizeof(TilemapVisualEntry);
    extensionBytes += _shadowMasks.size();
    const uint64_t total = sizeof(TilemapBinaryHeader) + flagsBytes
                         + tileIdsBytes + animBytes + extensionBytes;

    outData.resize(static_cast<size_t>(total));
    UInt8* ptr = outData.data();

    TilemapBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic               = ITilemap::MAGIC;
    header.version             = static_cast<UInt16>(ITilemap::VERSION);
    header.tileWidth           = _tileWidth;
    header.tileHeight          = _tileHeight;
    header.cols                 = _cols;
    header.rows                 = _rows;
    header.mode                 = static_cast<UInt8>(_mode);
    header.reserved             = 0;
    header.defaultTileId        = _defaultTileId;
    header.collisionFlagsCount  = static_cast<UInt32>(_tileCollisionFlags.size());
    header.tileIdsCount         = tileIdsCount;
    std::memcpy(ptr, &header, sizeof(header));

    size_t offset = sizeof(TilemapBinaryHeader);
    if (!_tileCollisionFlags.empty()) {
        std::memcpy(ptr + offset, _tileCollisionFlags.data(),
                    static_cast<size_t>(flagsBytes));
        offset += static_cast<size_t>(flagsBytes);
    }

    if (_mode == TilemapPackMode::Narrow16) {
        if (!_tileIds16.empty()) {
            std::memcpy(ptr + offset, _tileIds16.data(),
                        static_cast<size_t>(tileIdsBytes));
        }
    } else {
        if (!_tileIds32.empty()) {
            std::memcpy(ptr + offset, _tileIds32.data(),
                        static_cast<size_t>(tileIdsBytes));
        }
    }
    offset += static_cast<size_t>(tileIdsBytes);

    // Animation segment (always present, possibly empty).
    const UInt32 animCount = static_cast<UInt32>(_animations.size());
    std::memcpy(ptr + offset, &animCount, sizeof(animCount));
    offset += sizeof(animCount);
    for (const StoredAnimationEntry& e : _animations) {
        const UInt32 frameCount = static_cast<UInt32>(e.frames.size());
        std::memcpy(ptr + offset, &e.sourceTileId, sizeof(UInt32));
        std::memcpy(ptr + offset + sizeof(UInt32), &frameCount, sizeof(UInt32));
        offset += sizeof(UInt32) * 2;
        if (frameCount > 0) {
            std::memcpy(ptr + offset, e.frames.data(),
                        e.frames.size() * sizeof(TileAnimationFrame));
            offset += e.frames.size() * sizeof(TileAnimationFrame);
        }
    }

    TilemapV3ExtensionHeader extension{};
    extension.magic = kTilemapV3Magic;
    extension.layerCount = getLayerCount();
    extension.atlasCount = static_cast<UInt32>(_atlases.size());
    extension.visualCount = static_cast<UInt32>(_visuals.size());
    extension.shadowColorRgba = _shadowColorRgba;
    extension.shadowMaskCount = static_cast<UInt32>(_shadowMasks.size());
    extension.baseLayerVisible = _baseLayerVisible ? 1u : 0u;
    std::memcpy(ptr + offset, &extension, sizeof(extension));
    offset += sizeof(extension);
    for (const StoredLayer& layer : _extraLayers) {
        TilemapV3LayerHeader layerHeader{};
        layerHeader.visible = layer.visible ? 1u : 0u;
        layerHeader.tileIdCount = static_cast<UInt32>(
            _mode == TilemapPackMode::Narrow16
                ? layer.tileIds16.size() : layer.tileIds32.size());
        std::memcpy(ptr + offset, &layerHeader, sizeof(layerHeader));
        offset += sizeof(layerHeader);
        const size_t bytes = static_cast<size_t>(layerHeader.tileIdCount)
                           * static_cast<size_t>(elemSize);
        if (bytes > 0u) {
            const void* source = _mode == TilemapPackMode::Narrow16
                ? static_cast<const void*>(layer.tileIds16.data())
                : static_cast<const void*>(layer.tileIds32.data());
            std::memcpy(ptr + offset, source, bytes);
            offset += bytes;
        }
    }
    for (const StoredAtlasEntry& atlas : _atlases) {
        TilemapV3AtlasHeader atlasHeader{};
        atlasHeader.atlasId = atlas.atlasId;
        atlasHeader.imageWidth = atlas.imageWidth;
        atlasHeader.imageHeight = atlas.imageHeight;
        atlasHeader.pathBytes = static_cast<UInt32>(atlas.sourcePath.size());
        std::memcpy(ptr + offset, &atlasHeader, sizeof(atlasHeader));
        offset += sizeof(atlasHeader);
        if (!atlas.sourcePath.empty()) {
            std::memcpy(ptr + offset, atlas.sourcePath.data(),
                        atlas.sourcePath.size());
            offset += atlas.sourcePath.size();
        }
    }
    if (!_visuals.empty()) {
        const size_t bytes = _visuals.size() * sizeof(TilemapVisualEntry);
        std::memcpy(ptr + offset, _visuals.data(), bytes);
        offset += bytes;
    }
    if (!_shadowMasks.empty()) {
        std::memcpy(ptr + offset, _shadowMasks.data(), _shadowMasks.size());
        offset += _shadowMasks.size();
    }

    return true;
}

// ===== Create test data =====

void TilemapAsset::create(UInt32 cols, UInt32 rows,
                     UInt16 tileWidth, UInt16 tileHeight,
                     TilemapPackMode mode, UInt32 defaultTileId,
                     const TileCollisionFlagEntry* flags, UInt32 flagCount) {
    clear();
    _cols = cols;
    _rows = rows;
    _tileWidth = tileWidth;
    _tileHeight = tileHeight;
    _mode = mode;
    _defaultTileId = defaultTileId;

    const UInt32 count = cols * rows;
    if (mode == TilemapPackMode::Narrow16) {
        _tileIds16.assign(count, static_cast<UInt16>(
            defaultTileId > 0xFFFFu ? 0u : defaultTileId));
    } else {
        _tileIds32.assign(count, defaultTileId);
    }

    if (flags != nullptr && flagCount > 0) {
        _tileCollisionFlags.assign(flags, flags + flagCount);
    }

    _loaded = true;
}

bool TilemapAsset::setTile(UInt32 cellIndex, UInt32 tileId) {
    if (_mode == TilemapPackMode::Narrow16) {
        if (cellIndex >= _tileIds16.size() || tileId > 0xFFFFu) {
            return false;
        }
        _tileIds16[cellIndex] = static_cast<UInt16>(tileId);
        return true;
    }
    if (cellIndex >= _tileIds32.size()) {
        return false;
    }
    _tileIds32[cellIndex] = tileId;
    return true;
}

UInt32 TilemapAsset::getLayerCount() const {
    return 1u + static_cast<UInt32>(_extraLayers.size());
}

bool TilemapAsset::isLayerVisible(UInt32 layerIndex) const {
    if (layerIndex == 0u) return _baseLayerVisible;
    const UInt32 extraIndex = layerIndex - 1u;
    return extraIndex < _extraLayers.size()
        ? _extraLayers[extraIndex].visible : false;
}

const UInt16* TilemapAsset::getLayerTileIds16(UInt32 layerIndex) const {
    if (_mode != TilemapPackMode::Narrow16) return nullptr;
    if (layerIndex == 0u) return _tileIds16.empty() ? nullptr : _tileIds16.data();
    const UInt32 extraIndex = layerIndex - 1u;
    return extraIndex < _extraLayers.size()
        && !_extraLayers[extraIndex].tileIds16.empty()
            ? _extraLayers[extraIndex].tileIds16.data() : nullptr;
}

const UInt32* TilemapAsset::getLayerTileIds32(UInt32 layerIndex) const {
    if (_mode != TilemapPackMode::Wide32) return nullptr;
    if (layerIndex == 0u) return _tileIds32.empty() ? nullptr : _tileIds32.data();
    const UInt32 extraIndex = layerIndex - 1u;
    return extraIndex < _extraLayers.size()
        && !_extraLayers[extraIndex].tileIds32.empty()
            ? _extraLayers[extraIndex].tileIds32.data() : nullptr;
}

UInt32 TilemapAsset::getLayerTileIdCount(UInt32 layerIndex) const {
    if (layerIndex == 0u) return getTileIdCount();
    const UInt32 extraIndex = layerIndex - 1u;
    if (extraIndex >= _extraLayers.size()) return 0u;
    return static_cast<UInt32>(_mode == TilemapPackMode::Narrow16
        ? _extraLayers[extraIndex].tileIds16.size()
        : _extraLayers[extraIndex].tileIds32.size());
}

bool TilemapAsset::setLayer(UInt32 layerIndex, bool visible,
                            const UInt32* tileIds, UInt32 tileIdCount) {
    const UInt32 expected = getTileIdCount();
    if (tileIdCount > expected || (tileIdCount > 0u && tileIds == nullptr)
        || layerIndex > getLayerCount()) {
        return false;
    }
    const auto fill16 = [&](std::vector<UInt16>& target) {
        target.assign(expected, static_cast<UInt16>(_defaultTileId));
        for (UInt32 i = 0u; i < tileIdCount; ++i) {
            if (tileIds[i] > 0xffffu) return false;
            target[i] = static_cast<UInt16>(tileIds[i]);
        }
        return true;
    };
    const auto fill32 = [&](std::vector<UInt32>& target) {
        target.assign(expected, _defaultTileId);
        if (tileIdCount > 0u) {
            std::copy(tileIds, tileIds + tileIdCount, target.begin());
        }
        return true;
    };
    if (layerIndex == 0u) {
        _baseLayerVisible = visible;
        return _mode == TilemapPackMode::Narrow16
            ? fill16(_tileIds16) : fill32(_tileIds32);
    }
    const UInt32 extraIndex = layerIndex - 1u;
    if (extraIndex == _extraLayers.size()) _extraLayers.emplace_back();
    StoredLayer& layer = _extraLayers[extraIndex];
    layer.visible = visible;
    return _mode == TilemapPackMode::Narrow16
        ? fill16(layer.tileIds16) : fill32(layer.tileIds32);
}

bool TilemapAsset::addAtlasSource(UInt32 atlasId,
                                  const std::string& sourcePath,
                                  UInt32 imageWidth, UInt32 imageHeight) {
    if (atlasId == 0u || sourcePath.empty() || imageWidth == 0u
        || imageHeight == 0u || sourcePath.size() > 1024u * 1024u) {
        return false;
    }
    for (const StoredAtlasEntry& entry : _atlases) {
        if (entry.atlasId == atlasId) return false;
    }
    _atlases.push_back({atlasId, sourcePath, imageWidth, imageHeight});
    _atlasViewDirty = true;
    return true;
}

bool TilemapAsset::addTileVisual(const TilemapVisualEntry& visual) {
    if (visual.atlasId == 0u || visual.sourceWidth == 0u
        || visual.sourceHeight == 0u) {
        return false;
    }
    const StoredAtlasEntry* atlas = nullptr;
    for (const StoredAtlasEntry& candidate : _atlases) {
        if (candidate.atlasId == visual.atlasId) {
            atlas = &candidate;
            break;
        }
    }
    if (atlas == nullptr
        || static_cast<uint64_t>(visual.sourceX) + visual.sourceWidth
            > atlas->imageWidth
        || static_cast<uint64_t>(visual.sourceY) + visual.sourceHeight
            > atlas->imageHeight) {
        return false;
    }
    for (const TilemapVisualEntry& entry : _visuals) {
        if (entry.tileId == visual.tileId) return false;
    }
    _visuals.push_back(visual);
    return true;
}

bool TilemapAsset::setShadowData(UInt32 colorRgba, const UInt8* masks,
                                 UInt32 maskCount) {
    if (maskCount != 0u && (maskCount != getTileIdCount() || masks == nullptr)) {
        return false;
    }
    _shadowColorRgba = colorRgba;
    if (maskCount == 0u) {
        _shadowMasks.clear();
    } else {
        _shadowMasks.assign(masks, masks + maskCount);
    }
    return true;
}

UInt32 TilemapAsset::getAtlasCount() const {
    return static_cast<UInt32>(_atlases.size());
}

const TilemapAtlasEntry* TilemapAsset::getAtlasEntries() const {
    if (_atlasViewDirty) {
        _atlasEntriesView.clear();
        _atlasEntriesView.reserve(_atlases.size());
        for (const StoredAtlasEntry& atlas : _atlases) {
            _atlasEntriesView.push_back({atlas.atlasId,
                atlas.sourcePath.c_str(), atlas.imageWidth, atlas.imageHeight});
        }
        _atlasViewDirty = false;
    }
    return _atlasEntriesView.empty() ? nullptr : _atlasEntriesView.data();
}

// ===== Animation table (CM-5) =====

bool TilemapAsset::setAnimationEntry(UInt32 sourceTileId,
                                     const TileAnimationFrame* frames,
                                     UInt32 frameCount) {
    if (frameCount == 0) {
        // Removal. Absent entry is a no-op success (the table stays as it is).
        for (size_t i = 0; i < _animations.size(); ++i) {
            if (_animations[i].sourceTileId == sourceTileId) {
                _animations.erase(_animations.begin() + static_cast<ptrdiff_t>(i));
                _animViewDirty = true;
                return true;
            }
        }
        return true;
    }
    if (frames == nullptr) {
        return false;
    }
    // Replace if present, append otherwise (sparse table keeps no order
    // guarantees — consumers scan by sourceTileId).
    for (StoredAnimationEntry& e : _animations) {
        if (e.sourceTileId == sourceTileId) {
            e.frames.assign(frames, frames + frameCount);
            _animViewDirty = true;
            return true;
        }
    }
    StoredAnimationEntry e;
    e.sourceTileId = sourceTileId;
    e.frames.assign(frames, frames + frameCount);
    _animations.push_back(std::move(e));
    _animViewDirty = true;
    return true;
}

UInt32 TilemapAsset::getAnimationCount() const {
    getAnimationEntries();  // force the lazy view rebuild
    return static_cast<UInt32>(_animEntriesView.size());
}

const TileAnimationEntry* TilemapAsset::getAnimationEntries() const {
    if (_animViewDirty) {
        _animViewDirty = false;
        _animFlat.clear();
        _animEntriesView.clear();

        // Reserve the final flat size up front. Publishing view.frames from
        // _animFlat.data() while a later insert may reallocate would leave
        // earlier entries dangling (UAF reads → 0xDDDDDDDD under MSVC debug).
        size_t totalFrames = 0;
        for (const StoredAnimationEntry& e : _animations) {
            totalFrames += e.frames.size();
        }
        _animFlat.reserve(totalFrames);
        _animEntriesView.reserve(_animations.size());

        for (const StoredAnimationEntry& e : _animations) {
            const size_t base = _animFlat.size();
            _animFlat.insert(_animFlat.end(), e.frames.begin(), e.frames.end());
            TileAnimationEntry view;
            view.sourceTileId = e.sourceTileId;
            view.frameCount   = static_cast<UInt32>(e.frames.size());
            view.frames = e.frames.empty() ? nullptr : _animFlat.data() + base;
            _animEntriesView.push_back(view);
        }
    }
    return _animEntriesView.empty() ? nullptr : _animEntriesView.data();
}

} // namespace ayt::resource
