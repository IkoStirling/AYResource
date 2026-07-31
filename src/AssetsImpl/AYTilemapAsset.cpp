#include "AYTilemapAsset.h"
#include <ayio/File.h>
#include <cstring>
#include <cstdint>

namespace ayt::resource
{

// ===== .aytilemap binary format (v1) =====
//
// All multi-byte fields are host-endian little-endian (matches the AYPhysics
// / AYAudio precedent; the engine targets little-endian runtimes). The header
// is packed; the payload follows contiguously.
//
//   offset  field           notes
//   0       UInt32 magic    = 0x4D545941 ('AYTM')
//   4       UInt16 version  = 1
//   6       UInt16 tileWidth
//   8       UInt16 tileHeight
//  10       UInt32 cols
//  14       UInt32 rows
//  18       UInt8  mode     // 0 = Narrow16, 1 = Wide32
//  19       UInt8  reserved = 0
//  20       UInt32 defaultTileId
//  24       UInt32 blockedCount
//  28       UInt32 tileIdsCount   // = cols * rows
//  32       UInt32 blockedTileIds[blockedCount]   (omitted if 0)
//  32+4*blockedCount
//           UInt16 tileIds[tileIdsCount]  (Narrow16)  OR
//           UInt32 tileIds[tileIdsCount]  (Wide32)
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
    UInt32 blockedCount;
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
    _blockedTileIds.clear();
    _name.clear();
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
    return sizeof(TilemapAsset) + tileBytes
         + _blockedTileIds.size() * sizeof(UInt32) + _name.size();
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

    if (header->magic != ITilemap::MAGIC || header->version != ITilemap::VERSION) {
        return false;
    }

    // Validate pack mode.
    const UInt8 modeVal = header->mode;
    if (modeVal > static_cast<UInt8>(TilemapPackMode::Wide32)) {
        return false;
    }
    const TilemapPackMode mode = static_cast<TilemapPackMode>(modeVal);

    // Bounds-check the payload. tileIdsCount * elemSize can be up to
    // (2^32-1)*4 which overflows uint32; compute in uint64.
    const uint64_t blockedBytes =
        static_cast<uint64_t>(header->blockedCount) * sizeof(UInt32);
    const uint64_t elemSize = (mode == TilemapPackMode::Narrow16)
        ? static_cast<uint64_t>(sizeof(UInt16))
        : static_cast<uint64_t>(sizeof(UInt32));
    const uint64_t tileIdsBytes =
        static_cast<uint64_t>(header->tileIdsCount) * elemSize;
    const uint64_t headerBytes = sizeof(TilemapBinaryHeader);
    if (blockedBytes > size - headerBytes) return false;
    if (tileIdsBytes > (size - headerBytes) - blockedBytes) return false;

    clear();

    _tileWidth      = header->tileWidth;
    _tileHeight     = header->tileHeight;
    _cols           = header->cols;
    _rows           = header->rows;
    _mode           = mode;
    _defaultTileId  = header->defaultTileId;

    // Blocked tile ids.
    size_t offset = headerBytes;
    if (header->blockedCount > 0) {
        _blockedTileIds.resize(header->blockedCount);
        std::memcpy(_blockedTileIds.data(), ptr + offset,
                    static_cast<size_t>(blockedBytes));
        offset += static_cast<size_t>(blockedBytes);
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

    _loaded = true;
    return true;
}

bool TilemapAsset::saveToBinary(std::vector<UInt8>& outData) const {
    const UInt32 tileIdsCount = getTileIdCount();
    const uint64_t blockedBytes =
        static_cast<uint64_t>(_blockedTileIds.size()) * sizeof(UInt32);
    const uint64_t elemSize = (_mode == TilemapPackMode::Narrow16)
        ? static_cast<uint64_t>(sizeof(UInt16))
        : static_cast<uint64_t>(sizeof(UInt32));
    const uint64_t tileIdsBytes =
        static_cast<uint64_t>(tileIdsCount) * elemSize;
    const uint64_t total = sizeof(TilemapBinaryHeader) + blockedBytes + tileIdsBytes;

    outData.resize(static_cast<size_t>(total));
    UInt8* ptr = outData.data();

    TilemapBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic          = ITilemap::MAGIC;
    header.version        = static_cast<UInt16>(ITilemap::VERSION);
    header.tileWidth      = _tileWidth;
    header.tileHeight     = _tileHeight;
    header.cols           = _cols;
    header.rows           = _rows;
    header.mode           = static_cast<UInt8>(_mode);
    header.reserved       = 0;
    header.defaultTileId  = _defaultTileId;
    header.blockedCount   = static_cast<UInt32>(_blockedTileIds.size());
    header.tileIdsCount   = tileIdsCount;
    std::memcpy(ptr, &header, sizeof(header));

    size_t offset = sizeof(TilemapBinaryHeader);
    if (!_blockedTileIds.empty()) {
        std::memcpy(ptr + offset, _blockedTileIds.data(),
                    static_cast<size_t>(blockedBytes));
        offset += static_cast<size_t>(blockedBytes);
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

    return true;
}

// ===== Create test data =====

void TilemapAsset::create(UInt32 cols, UInt32 rows,
                     UInt16 tileWidth, UInt16 tileHeight,
                     TilemapPackMode mode, UInt32 defaultTileId,
                     const UInt32* blockedTileIds, UInt32 blockedCount) {
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

    if (blockedTileIds != nullptr && blockedCount > 0) {
        _blockedTileIds.assign(blockedTileIds, blockedTileIds + blockedCount);
    }

    _loaded = true;
}

} // namespace ayt::resource
