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
//   4       UInt16 version  = 2 (current). 1 = legacy bare blocked-id list.
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
// The animation segment is a single optional trailer: files written before
// CM-5 end exactly after the tile ids (empty table), the current writer
// always appends it (count may be 0), and the reader strictly consumes the
// tail — any leftover bytes after a full segment parse fail the load. This
// acts as a version lock: a future third segment must switch to four-cc
// chunks or bump the version (design.md §9.2), it must not be stacked here.
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
    return sizeof(TilemapAsset) + tileBytes
         + _tileCollisionFlags.size() * sizeof(TileCollisionFlagEntry)
         + animBytes + _name.size();
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
    // Accept v1 (legacy bare blocked-id list) and v2 (per-tile-id flags table).
    const UInt16 version = header->version;
    if (version != 1u && version != 2u) {
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
        if (offset != size) {
            clear();
            return false;  // strict tail consumption (version lock)
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
    const uint64_t total = sizeof(TilemapBinaryHeader) + flagsBytes + tileIdsBytes + animBytes;

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
