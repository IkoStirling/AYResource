#include "AYResource/Loader/TextureLoader.h"
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

namespace ayt::resource
{

namespace {

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
// here as single-level RGBA8 (the GPU upload path only consumes mip 0).
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
    if (fileSize == 0) {
        return nullptr;
    }
    std::vector<UInt8> fileBytes(fileSize);
    if (file.read(fileBytes.data(), fileSize) != fileSize) {
        return nullptr;
    }

    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char* pixels = stbi_load_from_memory(
        fileBytes.data(), static_cast<int>(fileBytes.size()),
        &w, &h, &comp, STBI_rgb_alpha);
    if (pixels == nullptr || w <= 0 || h <= 0) {
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

// ===== 常量 =====
// ===== 常量 =====

// ===== Texture 文件头 (二进制格式) =====
#pragma pack(push, 1)
struct TextureBinaryHeader {
    UInt32 magic;              // 'AYTX' = 0x48545841
    UInt16 version;            // 版本 = 1
    FGuid guid;                // 资源唯一标识 (16 bytes)
    UInt8  format;             // TextureFormat
    UInt8  mipmapCount;       // Mipmap 级别数量
    UInt32 width;              // 纹理宽度
    UInt32 height;             // 纹理高度
    UInt32 imageDataSize;      // 整个 imageData 大小
};
#pragma pack(pop)

// ===== Texture =====

Texture::Texture() = default;

void Texture::clear() {
    _imageData.clear();
    _mipOffsets.clear();
    _mipSizes.clear();
    _width = 0;
    _height = 0;
    _format = TextureFormat::RGBA8;
    _mipmapCount = 1;
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

    switch (format) {
        case TextureFormat::RGBA8:
            return width * height * 4;
        case TextureFormat::RGB8:
            return width * height * 3;
        case TextureFormat::BC1: {
            // DXT1: 8 bytes per 4x4 block
            UInt32 blocksWide = (width + 3) / 4;
            UInt32 blocksHigh = (height + 3) / 4;
            return blocksWide * blocksHigh * 8;
        }
        case TextureFormat::BC3: {
            // DXT5/BC3: 16 bytes per 4x4 block
            UInt32 blocksWide = (width + 3) / 4;
            UInt32 blocksHigh = (height + 3) / 4;
            return blocksWide * blocksHigh * 16;
        }
        case TextureFormat::BC4: {
            // BC4: 8 bytes per 4x4 block
            UInt32 blocksWide = (width + 3) / 4;
            UInt32 blocksHigh = (height + 3) / 4;
            return blocksWide * blocksHigh * 8;
        }
        case TextureFormat::BC5: {
            // BC5: 16 bytes per 4x4 block
            UInt32 blocksWide = (width + 3) / 4;
            UInt32 blocksHigh = (height + 3) / 4;
            return blocksWide * blocksHigh * 16;
        }
        case TextureFormat::BC7: {
            // BC7: 16 bytes per 4x4 block
            UInt32 blocksWide = (width + 3) / 4;
            UInt32 blocksHigh = (height + 3) / 4;
            return blocksWide * blocksHigh * 16;
        }
        default:
            return width * height * 4;
    }
}

bool Texture::load(const std::string& path) {
    _path = path;

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(TextureBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Texture::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(TextureBinaryHeader)) {
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const TextureBinaryHeader* header = reinterpret_cast<const TextureBinaryHeader*>(ptr);

    // 验证 magic
    if (header->magic != ITexture::MAGIC) {
        return false;
    }

    // 验证版本
    if (header->version != ITexture::VERSION) {
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取基础数据
    _width = header->width;
    _height = header->height;
    _format = static_cast<TextureFormat>(header->format);
    _mipmapCount = header->mipmapCount;

    // 验证尺寸
    if (_width == 0 || _height == 0 || _mipmapCount == 0) {
        return false;
    }

    // 计算每个 mip 的偏移和大小
    UInt32 currentWidth = _width;
    UInt32 currentHeight = _height;

    _mipOffsets.resize(_mipmapCount);
    _mipSizes.resize(_mipmapCount);

    UInt32 offset = 0;  // 数据在 _imageData 中从 0 开始
    for (UInt32 i = 0; i < _mipmapCount; i++) {
        UInt32 mipSize = computeMipSize(currentWidth, currentHeight, _format);
        _mipOffsets[i] = offset;
        _mipSizes[i] = mipSize;

        offset += mipSize;
        currentWidth = ayt::math::max(1u, currentWidth / 2);
        currentHeight = ayt::math::max(1u, currentHeight / 2);
    }

    // 验证数据完整性
    if (offset != header->imageDataSize) {
        return false;
    }

    if (header->imageDataSize > size - sizeof(TextureBinaryHeader)) {
        return false;
    }

    // 读取图像数据
    _imageData.resize(header->imageDataSize);
    std::memcpy(_imageData.data(), ptr + sizeof(TextureBinaryHeader), header->imageDataSize);

    _loaded = true;
    return true;
}

bool Texture::saveToBinary(std::vector<UInt8>& outData) const {
    // 计算总大小
    size_t totalSize = sizeof(TextureBinaryHeader) + _imageData.size();

    // 分配输出缓冲区
    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Header
    TextureBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = ITexture::MAGIC;
    header.version = ITexture::VERSION;
    header.guid = _guid;
    header.format = static_cast<UInt8>(_format);
    header.mipmapCount = static_cast<UInt8>(_mipmapCount);
    header.width = _width;
    header.height = _height;
    header.imageDataSize = static_cast<UInt32>(_imageData.size());

    std::memcpy(ptr, &header, sizeof(header));

    // 写入图像数据
    if (!_imageData.empty()) {
        std::memcpy(ptr + sizeof(TextureBinaryHeader), _imageData.data(), _imageData.size());
    }

    return true;
}

void Texture::createSolidColor(UInt32 width, UInt32 height, UInt32 r, UInt32 g, UInt32 b, UInt32 a) {
    clear();

    _width = width;
    _height = height;
    _format = TextureFormat::RGBA8;
    _mipmapCount = 1;

    UInt32 size = width * height * 4;
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

    _width = width;
    _height = height;
    _format = TextureFormat::RGBA8;
    _mipmapCount = 1;

    UInt32 size = width * height * 4;
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
    if (path.size() >= 6 && path.compare(path.size() - 6, 6, EXTENSION) == 0) {
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
    if (ext == ".aytex") {
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
