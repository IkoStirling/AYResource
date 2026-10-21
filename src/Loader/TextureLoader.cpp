#include "AYResource/Loader/TextureLoader.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYResource/IResourceLoader.h"
#include "AYMath/MathTypes.h"
#include "AYMath/MathUtils.h"
#include "AYIO/File.h"
// Dev loose png/jpg/... decode. Declaration only — STB_IMAGE_IMPLEMENTATION
// is defined once in Converter/TextureConverter.cpp (stb is a per-TU
// header; including the header here links against that implementation).
#include <stb_image.h>
#define NOMINMAX
#include <cctype>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ayt::resource
{

namespace {

constexpr UInt64 textureByteLimit = 256ull * 1024u * 1024u;

std::string lowerExtensionOf(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return {};
    }
    std::string ext = path.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

#if defined(AY_TEXTURE_LOOSE_FORMATS)
// Dev/editor authoring formats decodable by stb_image. Mirrors the
// AY_AUDIO_LOOSE_FORMATS gating: cook pipeline owns .aytex; dev texture
// references keep the source extension (.png/.jpg/...) and are decoded
// here as single-level RGBA8. Cooked .txr files may provide a complete mip chain.
bool isLooseTextureExtension(const std::string& ext)
{
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
        || ext == ".bmp" || ext == ".tga";
}

std::shared_ptr<Texture> loadLooseImage(const std::string& path)
{
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return nullptr;
    }
    const size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > textureByteLimit
        || fileSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return nullptr;
    }
    std::vector<UInt8> fileBytes(fileSize);
    if (file.read(fileBytes.data(), fileSize) != fileSize) {
        return nullptr;
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    if (!stbi_info_from_memory(fileBytes.data(),static_cast<int>(fileBytes.size()),&w,&h,&comp)
        || w <= 0 || h <= 0 || UInt64(w)*h*4u > textureByteLimit) return nullptr;
    unsigned char* pixels = stbi_load_from_memory(
        fileBytes.data(), static_cast<int>(fileBytes.size()),
        &w, &h, &comp, STBI_rgb_alpha);
    if (pixels == nullptr || w <= 0 || h <= 0 || UInt64(w)*h*4u > textureByteLimit) {
        stbi_image_free(pixels);
        return nullptr;
    }

    auto texture = std::make_shared<Texture>();
    texture->_width  = static_cast<UInt32>(w);
    texture->_height = static_cast<UInt32>(h);
    texture->_format = TextureFormat::RGBA8;
    texture->_mipmapCount = 1;
    const UInt32 pixelBytes =
        static_cast<UInt32>(w) * static_cast<UInt32>(h) * 4;
    texture->setImageData(pixels, pixelBytes);
    stbi_image_free(pixels);
    texture->setMipmapLayout(std::vector<UInt32>{0},
                             std::vector<UInt32>{pixelBytes});
    texture->setLoaded(true);
    return texture;
}
#endif

} // namespace

// ===== Texture 文件头 (二进制格式) =====
#pragma pack(push, 1)
struct TextureBinaryHeader {
    UInt32 magic;              // Historical numeric magic 0x48545841 (unchanged).
    UInt16 version;            // Readers accept v1/v2; writers use ITexture::VERSION.
    FGuid guid;                // 资源唯一标识 (16 bytes)
    UInt8  format;             // TextureFormat
    UInt8  mipmapCount;       // Mipmap 级别数量
    UInt32 width;              // 纹理宽度
    UInt32 height;             // 纹理高度
    UInt32 imageDataSize;      // 整个 imageData 大小
};
// v2: 16 bytes immediately after the unchanged 36-byte v1 header. No C++
// bool/enum padding is serialized. Image bytes follow this sampler record.
struct TextureSamplerBinary {
    UInt8 minFilter, magFilter, wrapU, wrapV, wrapW, srgb;
    UInt16 reserved;
    Float32 anisotropy, lodBias;
};
#pragma pack(pop)
static_assert(sizeof(TextureBinaryHeader) == 36);
static_assert(sizeof(TextureSamplerBinary) == 16);

namespace {
bool validSampler(const TextureSampler& sampler) {
    return static_cast<UInt8>(sampler.minFilter) <= static_cast<UInt8>(TextureFilter::Anisotropic)
        && static_cast<UInt8>(sampler.magFilter) <= static_cast<UInt8>(TextureFilter::Anisotropic)
        && static_cast<UInt8>(sampler.wrapU) <= static_cast<UInt8>(TextureWrap::Border)
        && static_cast<UInt8>(sampler.wrapV) <= static_cast<UInt8>(TextureWrap::Border)
        && static_cast<UInt8>(sampler.wrapW) <= static_cast<UInt8>(TextureWrap::Border)
        && std::isfinite(sampler.anisotropy) && sampler.anisotropy >= 1 && sampler.anisotropy <= 16
        && std::isfinite(sampler.lodBias) && sampler.lodBias >= -16 && sampler.lodBias <= 16;
}
UInt32 fullMipCount(UInt32 width, UInt32 height) {
    UInt32 count = 1;
    for (UInt32 side = std::max(width,height); side > 1; side >>= 1) ++count;
    return count;
}
} // namespace

// ===== Texture =====

Texture::Texture() = default;

void Texture::clear() {
    _loaded = false;
    _imageData.clear();
    _mipOffsets.clear();
    _mipSizes.clear();
    _width = 0;
    _height = 0;
    _format = TextureFormat::RGBA8;
    _mipmapCount = 1;
    _sampler = {};
}

bool Texture::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t Texture::sizeInBytes() const {
    return sizeof(Texture) + _imageData.size();
}

UInt32 Texture::computeMipSize(UInt32 width, UInt32 height, TextureFormat format) {
    if (width == 0 || height == 0) return 0;
    UInt64 units = UInt64(width)*height;
    UInt32 unitBytes = 0;
    const UInt64 blocks = ((UInt64(width)+3)/4) * ((UInt64(height)+3)/4);
    switch (format) {
    case TextureFormat::RGBA8: unitBytes = 4; break;
    case TextureFormat::RGB8: unitBytes = 3; break;
    case TextureFormat::BC1:
    case TextureFormat::BC4: units = blocks; unitBytes = 8; break;
    case TextureFormat::BC3:
    case TextureFormat::BC5:
    case TextureFormat::BC7: units = blocks; unitBytes = 16; break;
    default: return 0;
    }
    if (units > std::numeric_limits<UInt32>::max()/unitBytes) return 0;
    return static_cast<UInt32>(units*unitBytes);
}

bool Texture::load(const std::string& path) {
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(TextureBinaryHeader)
        || fileSize > textureByteLimit + sizeof(TextureBinaryHeader) + sizeof(TextureSamplerBinary)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    if (!loadFromBinary(data.data(), data.size())) return false;
    _path = path;
    return true;
}

bool Texture::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(TextureBinaryHeader)) {
        return false;
    }

    const UInt8* ptr = static_cast<const UInt8*>(data);
    TextureBinaryHeader header{};
    std::memcpy(&header,ptr,sizeof(header));
    if (header.magic != ITexture::MAGIC || (header.version != 1 && header.version != ITexture::VERSION)
        || !header.width || !header.height || !header.mipmapCount
        || header.mipmapCount > fullMipCount(header.width,header.height)
        || header.imageDataSize > textureByteLimit) return false;
    const size_t imageOffset = sizeof(header) + (header.version == 1 ? 0 : sizeof(TextureSamplerBinary));
    if (size < imageOffset || header.imageDataSize != size - imageOffset) return false;
    TextureSampler sampler;
    if (header.version != 1) {
        TextureSamplerBinary encoded{};
        std::memcpy(&encoded,ptr+sizeof(header),sizeof(encoded));
        if (encoded.reserved || encoded.srgb > 1) return false;
        sampler.minFilter = static_cast<TextureFilter>(encoded.minFilter);
        sampler.magFilter = static_cast<TextureFilter>(encoded.magFilter);
        sampler.wrapU = static_cast<TextureWrap>(encoded.wrapU);
        sampler.wrapV = static_cast<TextureWrap>(encoded.wrapV);
        sampler.wrapW = static_cast<TextureWrap>(encoded.wrapW);
        sampler.sRGB = encoded.srgb != 0;
        sampler.anisotropy = encoded.anisotropy; sampler.lodBias = encoded.lodBias;
        if (!validSampler(sampler)) return false;
    }
    UInt32 width = header.width, height = header.height;
    UInt64 offset = 0;
    std::vector<UInt32> offsets,sizes;
    for (UInt32 level = 0; level < header.mipmapCount; ++level) {
        const auto bytes = computeMipSize(width,height,static_cast<TextureFormat>(header.format));
        if (!bytes || offset + bytes > header.imageDataSize) return false;
        offsets.push_back(static_cast<UInt32>(offset)); sizes.push_back(bytes); offset += bytes;
        width = std::max(1u,width/2); height = std::max(1u,height/2);
    }
    if (offset != header.imageDataSize) return false;
    std::vector<UInt8> image(ptr+imageOffset,ptr+size);
    // Commit only after every header, sampler and level has been validated.
    // Failed decode preserves both prior image bytes and authoring settings.
    _guid = header.guid; _width = header.width; _height = header.height;
    _format = static_cast<TextureFormat>(header.format); _mipmapCount = header.mipmapCount;
    _sampler = sampler; _imageData = std::move(image);
    _mipOffsets = std::move(offsets); _mipSizes = std::move(sizes);
    _loaded = true;
    return true;
}

bool Texture::saveToBinary(std::vector<UInt8>& outData) const {
    const auto count = getMipmapCount();
    if (!_width || !_height || !count || _mipSizes.size() != count || count > fullMipCount(_width,_height)
        || _imageData.size() > textureByteLimit || !validSampler(_sampler)) return false;
    UInt32 width = _width, height = _height;
    size_t offset = 0;
    for (UInt32 level = 0; level < count; ++level) {
        const auto bytes = computeMipSize(width,height,_format);
        if (!bytes || level >= _mipSizes.size() || _mipOffsets[level] != offset
            || getMipmapSize(level) != bytes || !getMipmapData(level)) return false;
        offset += bytes; width = std::max(1u,width/2); height = std::max(1u,height/2);
    }
    if (offset != _imageData.size()) return false;
    const size_t totalSize = sizeof(TextureBinaryHeader) + sizeof(TextureSamplerBinary) + _imageData.size();
    std::vector<UInt8> output(totalSize);
    UInt8* ptr = output.data();

    // 写入 Header
    TextureBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = ITexture::MAGIC;
    header.version = ITexture::VERSION;
    header.guid = _guid;
    header.format = static_cast<UInt8>(_format);
    header.mipmapCount = static_cast<UInt8>(count);
    header.width = _width;
    header.height = _height;
    header.imageDataSize = static_cast<UInt32>(_imageData.size());

    std::memcpy(ptr, &header, sizeof(header));
    const TextureSamplerBinary sampler{
        static_cast<UInt8>(_sampler.minFilter), static_cast<UInt8>(_sampler.magFilter),
        static_cast<UInt8>(_sampler.wrapU), static_cast<UInt8>(_sampler.wrapV), static_cast<UInt8>(_sampler.wrapW),
        static_cast<UInt8>(_sampler.sRGB ? 1 : 0), 0, _sampler.anisotropy, _sampler.lodBias};
    std::memcpy(ptr+sizeof(header),&sampler,sizeof(sampler));

    // 写入图像数据
    if (!_imageData.empty()) {
        std::memcpy(ptr + sizeof(header) + sizeof(sampler), _imageData.data(), _imageData.size());
    }
    outData = std::move(output);
    return true;
}

void Texture::createSolidColor(UInt32 width, UInt32 height, UInt32 r, UInt32 g, UInt32 b, UInt32 a) {
    clear();
    const UInt32 size = computeMipSize(width,height,TextureFormat::RGBA8);
    if (!size || size > textureByteLimit) return;

    _width = width;
    _height = height;
    _format = TextureFormat::RGBA8;
    _mipmapCount = 1;

    _imageData.resize(size);
    _mipOffsets.resize(1, 0);
    _mipSizes.resize(1, size);

    UInt8* ptr = _imageData.data();
    for (UInt32 i = 0; i < width * height; i++) {
        ptr[i * 4 + 0] = static_cast<UInt8>(r);
        ptr[i * 4 + 1] = static_cast<UInt8>(g);
        ptr[i * 4 + 2] = static_cast<UInt8>(b);
        ptr[i * 4 + 3] = static_cast<UInt8>(a);
    }

    _loaded = true;
}

void Texture::createCheckerboard(UInt32 width, UInt32 height, UInt32 checkSize) {
    clear();
    const UInt32 size = computeMipSize(width,height,TextureFormat::RGBA8);
    if (!size || size > textureByteLimit || !checkSize) return;

    _width = width;
    _height = height;
    _format = TextureFormat::RGBA8;
    _mipmapCount = 1;

    _imageData.resize(size);
    _mipOffsets.resize(1, 0);
    _mipSizes.resize(1, size);

    UInt8* ptr = _imageData.data();
    for (UInt32 y = 0; y < height; y++) {
        for (UInt32 x = 0; x < width; x++) {
            bool isLight = (((x / checkSize) + (y / checkSize)) % 2 == 0);
            UInt32 i = y * width + x;
            if (isLight) {
                ptr[i * 4 + 0] = 255;
                ptr[i * 4 + 1] = 255;
                ptr[i * 4 + 2] = 255;
                ptr[i * 4 + 3] = 255;
            } else {
                ptr[i * 4 + 0] = 0;
                ptr[i * 4 + 1] = 0;
                ptr[i * 4 + 2] = 0;
                ptr[i * 4 + 3] = 255;
            }
        }
    }

    _loaded = true;
}

// ===== TextureLoader =====

bool TextureLoader::canLoad(const std::string& path) const {
    if (ayt::asset_format::matchesPath(path, ayt::asset_format::Id::Texture)) {
        return true;
    }
#if defined(AY_TEXTURE_LOOSE_FORMATS)
    return isLooseTextureExtension(lowerExtensionOf(path));
#else
    return false;
#endif
}

std::shared_ptr<IResource> TextureLoader::load(const std::string& path) {
    const std::string ext = lowerExtensionOf(path);
    if (ayt::asset_format::matchesPath(path, ayt::asset_format::Id::Texture)) {
        auto texture = std::make_shared<Texture>();
        if (texture->load(path)) {
            return texture;
        }
        return nullptr;
    }
#if defined(AY_TEXTURE_LOOSE_FORMATS)
    if (isLooseTextureExtension(ext)) {
        return loadLooseImage(path);
    }
#endif
    return nullptr;
}

std::shared_ptr<IResource> TextureLoader::loadFromBinary(const void* data, size_t size) {
    auto texture = std::make_shared<Texture>();
    if (texture->loadFromBinary(data, size)) {
        return texture;
    }
    return nullptr;
}

std::shared_ptr<IResource> TextureLoader::loadAsync(const std::string& path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
    auto texture = load(path);
    if (callback) {
        callback(texture);
    }
    return texture;
}

} // namespace ayt::resource
