#include "Converter\TextureConverter.h"
#include "Converter\TextureCompressor.h"
#include "Loader\TextureLoader.h"
#include "AYFile.h"
#include "AYMathUtils.h"
#include <AYGuid.h>
#include <cstring>

// stb_image for PNG/JPG loading
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace ayt::resource
{

// ===== BMP 文件头 (简化版, 仅支持 24-bit 和 32-bit) =====
#pragma pack(push, 1)
struct BMPFileHeader {
    UInt16 magic;              // 'BM' = 0x4D42
    UInt32 fileSize;           // 文件大小
    UInt16 reserved1;
    UInt16 reserved2;
    UInt32 dataOffset;         // 像素数据偏移
};

struct BMPInfoHeader {
    UInt32 headerSize;         // InfoHeader 大小 (40)
    Int32  width;              // 宽度
    Int32  height;             // 高度 (正数 = 底部朝上, 负数 = 顶部朝上)
    UInt16 planes;             // 平面数 (必须为 1)
    UInt16 bitsPerPixel;       // 每像素位数 (24 或 32)
    UInt32 compression;        // 压缩方式 (0 = 无压缩)
    UInt32 imageSize;          // 像素数据大小
    Int32  xPixelsPerMeter;
    Int32  yPixelsPerMeter;
    UInt32 colorsUsed;
    UInt32 colorsImportant;
};
#pragma pack(pop)

// ===== TGA 文件头 =====
#pragma pack(push, 1)
struct TGAHeader {
    UInt8  idLength;           // 图像信息 ID 长度
    UInt8  colorMapType;       // 颜色映射类型 (0 = 无颜色映射)
    UInt8  imageType;          // 图像类型 (2 = 无压缩真彩色, 10 = RLE真彩色)
    UInt16 colorMapOrigin;     // 颜色映射原点
    UInt16 colorMapLength;     // 颜色映射长度
    UInt8  colorMapDepth;      // 颜色映射深度
    UInt16 xOrigin;            // X 坐标原点
    UInt16 yOrigin;            // Y 坐标原点
    UInt16 width;              // 宽度
    UInt16 height;             // 高度
    UInt8  bitsPerPixel;       // 每像素位数 (24 或 32)
    UInt8  imageDescriptor;    // 图像描述符
};
#pragma pack(pop)

TextureConverter::TextureConverter() = default;

TextureConverter::TextureConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void TextureConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void TextureConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

// ===== 工具函数 =====
static bool writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

// 读取已有 aytex 文件的格式信息
// 返回 true 如果成功读取，format 会被设置为文件的压缩格式
static bool getAytexFormat(const std::string& path, UInt8& format) {
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) return false;

    struct AytexHeader {
        UInt32 magic;
        UInt16 version;
        UInt8 format;
        UInt8 mipmapCount;
        UInt32 width;
        UInt32 height;
        UInt32 imageDataSize;
    };

    AytexHeader header;
    if (file.read(&header, sizeof(header)) != sizeof(header)) {
        return false;
    }

    // 验证 magic ('AYTX' = 0x48545841)
    if (header.magic != 0x48545841) {
        return false;
    }

    format = header.format;
    return true;
}

static std::string getFileName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

static std::string getBaseName(const std::string& path) {
    std::string fileName = getFileName(path);
    size_t dotPos = fileName.find_last_of('.');
    if (dotPos == std::string::npos) {
        return fileName;
    }
    return fileName.substr(0, dotPos);
}

static std::string getExtension(const std::string& path) {
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) {
        return "";
    }
    return path.substr(dotPos + 1);
}

// 解析纹理路径为完整路径
static std::string resolveTexturePath(const std::string& texPath, const std::string& baseFbxDir) {
    // 如果已经是绝对路径，直接返回
    if (texPath.size() >= 2 && texPath[1] == ':') {
        return texPath;
    }
    if (texPath.size() >= 1 && texPath[0] == '/') {
        return texPath;
    }

    // 相对路径，拼接 baseFbxDir
    std::string dir = baseFbxDir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
        dir += '/';
    }
    return dir + texPath;
}

// ===== 加载 BMP 图像 =====
static bool loadBMP(const std::string& path, std::vector<UInt8>& pixels,
                    UInt32& width, UInt32& height, TextureFormat& format) {
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    BMPFileHeader fileHeader;
    BMPInfoHeader infoHeader;

    if (file.read(&fileHeader, sizeof(fileHeader)) != sizeof(fileHeader)) {
        return false;
    }

    // 验证 magic
    if (fileHeader.magic != 0x4D42) {  // 'BM'
        return false;
    }

    if (file.read(&infoHeader, sizeof(infoHeader)) != sizeof(infoHeader)) {
        return false;
    }

    // 只支持无压缩的 24-bit 或 32-bit
    if (infoHeader.compression != 0) {
        return false;
    }

    if (infoHeader.bitsPerPixel != 24 && infoHeader.bitsPerPixel != 32) {
        return false;
    }

    width = static_cast<UInt32>(infoHeader.width);
    height = static_cast<UInt32>(infoHeader.height < 0 ? -infoHeader.height : infoHeader.height);
    bool bottomUp = (infoHeader.height > 0);

    format = (infoHeader.bitsPerPixel == 32) ? TextureFormat::RGBA8 : TextureFormat::RGB8;

    // 计算行大小和像素数据大小
    UInt32 bytesPerPixel = infoHeader.bitsPerPixel / 8;
    UInt32 rowSize = (width * bytesPerPixel + 3) & ~3u;  // 4字节对齐
    size_t pixelDataSize = rowSize * height;

    // 跳到像素数据
    if (file.seek(static_cast<int64_t>(fileHeader.dataOffset), SEEK_SET) != 0) {
        return false;
    }

    std::vector<UInt8> rawData(pixelDataSize);
    if (file.read(rawData.data(), pixelDataSize) != pixelDataSize) {
        return false;
    }

    // 转换为 RGBA8 或 RGB8 (移除行对齐, 转换为连续内存)
    UInt32 outBytesPerPixel = (format == TextureFormat::RGBA8) ? 4 : 3;
    pixels.resize(width * height * outBytesPerPixel);

    for (UInt32 y = 0; y < height; y++) {
        UInt32 srcRow = bottomUp ? (height - 1 - y) : y;
        const UInt8* srcRowPtr = rawData.data() + srcRow * rowSize;
        UInt8* dstPtr = pixels.data() + y * width * outBytesPerPixel;

        for (UInt32 x = 0; x < width; x++) {
            const UInt8* src = srcRowPtr + x * bytesPerPixel;
            UInt8* dst = dstPtr + x * outBytesPerPixel;

            // BMP 是 BGR 顺序
            dst[0] = src[0];  // B
            dst[1] = src[1];  // G
            dst[2] = src[2];  // R

            if (format == TextureFormat::RGBA8) {
                dst[3] = (bytesPerPixel == 4) ? src[3] : 255;
            }
        }
    }

    return true;
}

// ===== 加载 TGA 图像 (仅支持无压缩真彩色) =====
static bool loadTGA(const std::string& path, std::vector<UInt8>& pixels,
                    UInt32& width, UInt32& height, TextureFormat& format) {
    ayt::io::File file(path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    TGAHeader header;
    if (file.read(&header, sizeof(header)) != sizeof(header)) {
        return false;
    }

    // 只支持类型 2 (无压缩真彩色)
    if (header.imageType != 2) {
        return false;
    }

    if (header.bitsPerPixel != 24 && header.bitsPerPixel != 32) {
        return false;
    }

    width = header.width;
    height = header.height;
    format = (header.bitsPerPixel == 32) ? TextureFormat::RGBA8 : TextureFormat::RGB8;

    UInt32 bytesPerPixel = header.bitsPerPixel / 8;
    size_t pixelDataSize = width * height * bytesPerPixel;

    // 跳过图像 ID
    if (header.idLength > 0) {
        file.seek(static_cast<int64_t>(header.idLength), SEEK_SET);
    }

    std::vector<UInt8> rawData(pixelDataSize);
    if (file.read(rawData.data(), pixelDataSize) != pixelDataSize) {
        return false;
    }

    // 转换为目标格式
    UInt32 outBytesPerPixel = (format == TextureFormat::RGBA8) ? 4 : 3;
    pixels.resize(width * height * outBytesPerPixel);

    // TGA 是自下而上, 自左至右
    for (UInt32 y = 0; y < height; y++) {
        const UInt8* srcRowPtr = rawData.data() + y * width * bytesPerPixel;
        UInt8* dstPtr = pixels.data() + y * width * outBytesPerPixel;

        for (UInt32 x = 0; x < width; x++) {
            const UInt8* src = srcRowPtr + x * bytesPerPixel;
            UInt8* dst = dstPtr + x * outBytesPerPixel;

            // TGA 是 BGR 顺序
            dst[0] = src[0];  // B
            dst[1] = src[1];  // G
            dst[2] = src[2];  // R

            if (format == TextureFormat::RGBA8) {
                dst[3] = (bytesPerPixel == 4) ? src[3] : 255;
            }
        }
    }

    return true;
}

// ===== 加载 PNG/JPG 图像（使用 stb_image）=====
static bool loadImage(const std::string& path, std::vector<UInt8>& pixels,
                      UInt32& width, UInt32& height, TextureFormat& format) {
    int w, h, channels;
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!data) {
        return false;
    }

    width = static_cast<UInt32>(w);
    height = static_cast<UInt32>(h);
    format = TextureFormat::RGBA8;

    size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    pixels.resize(pixelCount * 4);
    std::memcpy(pixels.data(), data, pixels.size());

    stbi_image_free(data);
    return true;
}

// ===== 生成 Mipmap =====
static void generateMipmapLevel(const UInt8* src, UInt32 srcWidth, UInt32 srcHeight,
                                 TextureFormat format, UInt8* dst) {
    UInt32 dstWidth = ayt::math::max(1u, srcWidth / 2);
    UInt32 dstHeight = ayt::math::max(1u, srcHeight / 2);

    UInt32 bytesPerPixel = (format == TextureFormat::RGBA8) ? 4 : 3;

    for (UInt32 y = 0; y < dstHeight; y++) {
        for (UInt32 x = 0; x < dstWidth; x++) {
            // 4 个相邻像素的平均值
            UInt32 sum[4] = {0, 0, 0, 0};
            for (Int32 dy = 0; dy < 2; dy++) {
                for (Int32 dx = 0; dx < 2; dx++) {
                    UInt32 sx = ayt::math::min(x * 2 + dx, srcWidth - 1);
                    UInt32 sy = ayt::math::min(y * 2 + dy, srcHeight - 1);
                    const UInt8* srcPixel = src + (sy * srcWidth + sx) * bytesPerPixel;
                    for (UInt32 c = 0; c < bytesPerPixel; c++) {
                        sum[c] += srcPixel[c];
                    }
                }
            }

            UInt8* dstPixel = dst + (y * dstWidth + x) * bytesPerPixel;
            for (UInt32 c = 0; c < bytesPerPixel; c++) {
                dstPixel[c] = static_cast<UInt8>(sum[c] / 4);
            }
        }
    }
}

ConversionResult TextureConverter::convertFromPath(const std::string& imagePath,
                                               const std::string& textureName,
                                               const std::string& baseFbxDir) {
    ConversionResult result;

    // 解析完整路径
    std::string fullPath = resolveTexturePath(imagePath, baseFbxDir);
    std::string ext = getExtension(fullPath);
    for (auto& c : ext) c = static_cast<char>(tolower(c));

    // 构建输出路径
    std::string outputFileName = textureName + usageSuffix + (passthrough ? ("." + ext) : ".aytex");
    std::string virtualPath = "textures/" + outputFileName;
    std::string fullOutputPath;
    if (!outputDir.empty()) {
        fullOutputPath = outputDir + "/" + virtualPath;
    }

    // 跳过已存在的文件（中断恢复）
    if (!fullOutputPath.empty() && ayt::io::File::exists(fullOutputPath)) {
        UInt8 existingFormat = 0;
        if (getAytexFormat(fullOutputPath, existingFormat)) {
            if (existingFormat == static_cast<UInt8>(outputFormat)) {
                printf("  [SKIP] %s (format matches, skip)\n", outputFileName.c_str());
                return result;
            } else {
                printf("  [REPLACE] %s (format changed, reconverting)\n", outputFileName.c_str());
            }
        }
    }

    printf("  [CONVERT] %s\n", outputFileName.c_str());

    // ===== 直接拷贝模式 =====
    if (passthrough) {
        // 跳过加载/解码/压缩，直接将源文件原始数据复制到输出目录
        // 输出文件扩展名保持为 .aytex，但内容是原始 PNG/JPG 等
        ayt::io::File srcFile(fullPath, ayt::io::File::Mode::BinaryRead);
        if (!srcFile.isOpen()) {
            return result;
        }
        size_t fileSize = srcFile.size();
        std::vector<UInt8> rawData(fileSize);
        if (srcFile.read(rawData.data(), fileSize) != fileSize) {
            return result;
        }

        if (!fullOutputPath.empty() && !ayt::io::File::exists(fullOutputPath)) {
            writeFile(fullOutputPath, rawData.data(), rawData.size());
        }

        ConversionResult::ConvertedResource res;
        res.path = virtualPath;
        res.type = "Texture";
        res.size = static_cast<int64_t>(fileSize);
        result.resources.push_back(res);
        return result;
    }

    // 加载源图像
    std::vector<UInt8> pixels;
    UInt32 width = 0;
    UInt32 height = 0;
    TextureFormat format = TextureFormat::RGBA8;

    bool loaded = false;
    if (ext == "bmp") {
        loaded = loadBMP(fullPath, pixels, width, height, format);
    } else if (ext == "tga") {
        loaded = loadTGA(fullPath, pixels, width, height, format);
    } else if (ext == "png" || ext == "jpg" || ext == "jpeg") {
        loaded = loadImage(fullPath, pixels, width, height, format);
    }

    if (!loaded) {
        return result;
    }

    // 创建 Texture
    Texture texture;
    texture._width = width;
    texture._height = height;
    texture._format = outputFormat;

    // 计算 mipmap
    UInt32 mipWidth = width;
    UInt32 mipHeight = height;
    UInt32 mipCount = 1;
    while (mipCount < 16 && (mipWidth > 1 || mipHeight > 1)) {
        mipWidth = ayt::math::max(1u, mipWidth / 2);
        mipHeight = ayt::math::max(1u, mipHeight / 2);
        mipCount++;
    }
    if (!generateMipmaps) {
        mipCount = 1;
    }

    texture._mipmapCount = mipCount;

    // 生成 RGBA mipmap 数据（先 RGBA 生成，后续压缩）
    std::vector<std::vector<UInt8>> mipRGBA(mipCount);
    std::vector<UInt32> mipRGBAWidth(mipCount);
    std::vector<UInt32> mipRGBAHeight(mipCount);

    mipRGBA[0] = pixels;
    mipRGBAWidth[0] = width;
    mipRGBAHeight[0] = height;

    mipWidth = width;
    mipHeight = height;
    for (UInt32 i = 1; i < mipCount; i++) {
        mipWidth = ayt::math::max(1u, mipWidth / 2);
        mipHeight = ayt::math::max(1u, mipHeight / 2);
        mipRGBAWidth[i] = mipWidth;
        mipRGBAHeight[i] = mipHeight;
        mipRGBA[i].resize(mipWidth * mipHeight * 4);
        generateMipmapLevel(mipRGBA[i - 1].data(), mipRGBAWidth[i - 1], mipRGBAHeight[i - 1],
                            TextureFormat::RGBA8, mipRGBA[i].data());
    }

    // 扁平化存储（如果是 BC 格式则压缩）
    size_t totalSize = 0;
    for (UInt32 i = 0; i < mipCount; i++) {
        totalSize += Texture::computeMipSize(mipRGBAWidth[i], mipRGBAHeight[i], outputFormat);
    }

    texture._imageData.resize(totalSize);
    texture._mipOffsets.resize(mipCount);
    texture._mipSizes.resize(mipCount);

    UInt8* dstPtr = texture._imageData.data();

    // 创建压缩器
    TextureCompressor::Format compFormat = (outputFormat == TextureFormat::BC7)
        ? TextureCompressor::Format::BC7
        : TextureCompressor::Format::BC3;
    auto compressor = TextureCompressor::create(compFormat);

    printf("  [INFO] Compressing %ux%u, %u mip levels with %s...\n",
           width, height, mipCount, compressor->getFormatName());

    for (UInt32 i = 0; i < mipCount; i++) {
        texture._mipOffsets[i] = static_cast<UInt32>(dstPtr - texture._imageData.data());

        if (outputFormat == TextureFormat::BC7 || outputFormat == TextureFormat::BC3 ||
            outputFormat == TextureFormat::BC1 || outputFormat == TextureFormat::BC5) {
            // 压缩格式：使用压缩器
            printf("    Mip[%u/%u]: %ux%u ...\n", i + 1, mipCount, mipRGBAWidth[i], mipRGBAHeight[i]);
            auto compressed = compressor->compress(mipRGBA[i].data(), mipRGBAWidth[i], mipRGBAHeight[i]);
            UInt32 compressedSize = static_cast<UInt32>(compressed.size());
            std::memcpy(dstPtr, compressed.data(), compressedSize);
            texture._mipSizes[i] = compressedSize;
            dstPtr += compressedSize;
        } else {
            // 非压缩格式：直接复制 RGBA 数据
            UInt32 dataSize = mipRGBAWidth[i] * mipRGBAHeight[i] * 4;
            std::memcpy(dstPtr, mipRGBA[i].data(), dataSize);
            texture._mipSizes[i] = dataSize;
            dstPtr += dataSize;
        }
    }

    texture._loaded = true;

    // 计算 GUID
    lastGuid = ayt::storage::Guid::computeFromData(texture._imageData.data(), texture._imageData.size());
    texture.setGuid(lastGuid);

    // 保存到二进制
    std::vector<UInt8> binaryData;
    if (!texture.saveToBinary(binaryData)) {
        return result;
    }

    // 立即写入（中断恢复支持）
    if (!fullOutputPath.empty()) {
        ayt::io::File::atomicWrite(fullOutputPath, binaryData.data(), binaryData.size());
    }

    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Texture";
    res.size = static_cast<int64_t>(binaryData.size());
    result.resources.push_back(res);
    return result;
}

ConversionResult TextureConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        return result;
    }

    // 构建输出路径
    std::string baseName = getFileName(sourcePath);
    std::string outputFileName = baseName + usageSuffix + ".aytex";
    std::string virtualPath = "textures/" + outputFileName;
    std::string fullOutputPath;
    if (!outputDir.empty()) {
        fullOutputPath = outputDir + "/" + virtualPath;
    }

    // 跳过已存在的文件（中断恢复）
    if (!fullOutputPath.empty() && ayt::io::File::exists(fullOutputPath)) {
        UInt8 existingFormat = 0;
        if (getAytexFormat(fullOutputPath, existingFormat)) {
            if (existingFormat == static_cast<UInt8>(outputFormat)) {
                printf("  [SKIP] %s (format matches, skip)\n", outputFileName.c_str());
                return result;
            } else {
                printf("  [REPLACE] %s (format changed, reconverting)\n", outputFileName.c_str());
            }
        }
    }

    std::string ext = getExtension(sourcePath);
    for (auto& c : ext) c = static_cast<char>(tolower(c));

    // 加载源图像
    std::vector<UInt8> pixels;
    UInt32 width = 0;
    UInt32 height = 0;
    TextureFormat format = TextureFormat::RGBA8;

    bool loaded = false;
    if (ext == "bmp") {
        loaded = loadBMP(sourcePath, pixels, width, height, format);
    } else if (ext == "tga") {
        loaded = loadTGA(sourcePath, pixels, width, height, format);
    } else if (ext == "png" || ext == "jpg" || ext == "jpeg") {
        loaded = loadImage(sourcePath, pixels, width, height, format);
    }

    if (!loaded) {
        return result;
    }

    // 创建 Texture
    Texture texture;
    texture._width = width;
    texture._height = height;
    texture._format = outputFormat;

    // 计算 mipmap 数量
    UInt32 mipWidth = width;
    UInt32 mipHeight = height;
    UInt32 mipCount = 1;
    while (mipWidth > 1 || mipHeight > 1) {
        mipWidth = ayt::math::max(1u, mipWidth / 2);
        mipHeight = ayt::math::max(1u, mipHeight / 2);
        mipCount++;
    }

    if (!generateMipmaps) {
        mipCount = 1;
        mipWidth = width;
        mipHeight = height;
    }

    texture._mipmapCount = mipCount;

    // 收集 mip 数据
    std::vector<std::vector<UInt8>> mipDatas(mipCount);
    std::vector<UInt32> mipSizes(mipCount);

    mipWidth = width;
    mipHeight = height;
    mipDatas[0] = pixels;
    mipSizes[0] = static_cast<UInt32>(pixels.size());

    for (UInt32 i = 1; i < mipCount; i++) {
        mipWidth = ayt::math::max(1u, mipWidth / 2);
        mipHeight = ayt::math::max(1u, mipHeight / 2);
        UInt32 mipSize = Texture::computeMipSize(mipWidth, mipHeight, texture._format);
        mipDatas[i].resize(mipSize);
        generateMipmapLevel(mipDatas[i - 1].data(), mipWidth * 2, mipHeight * 2,
                            texture._format, mipDatas[i].data());
        mipSizes[i] = mipSize;
    }

    // 扁平化存储到 imageData
    size_t totalSize = 0;
    for (UInt32 i = 0; i < mipCount; i++) {
        totalSize += mipSizes[i];
    }

    texture._imageData.resize(totalSize);
    texture._mipOffsets.resize(mipCount);
    texture._mipSizes.resize(mipCount);

    UInt8* dstPtr = texture._imageData.data();
    for (UInt32 i = 0; i < mipCount; i++) {
        texture._mipOffsets[i] = static_cast<UInt32>(dstPtr - texture._imageData.data());
        texture._mipSizes[i] = mipSizes[i];
        std::memcpy(dstPtr, mipDatas[i].data(), mipSizes[i]);
        dstPtr += mipSizes[i];
    }

    texture._loaded = true;

    // 计算 GUID
    lastGuid = ayt::storage::Guid::computeFromData(texture._imageData.data(), texture._imageData.size());
    texture.setGuid(lastGuid);

    // 保存到二进制数据
    std::vector<UInt8> binaryData;
    if (!texture.saveToBinary(binaryData)) {
        return result;
    }

    // 写入输出目录（立即写入，支持中断恢复）
    if (!fullOutputPath.empty()) {
        writeFile(fullOutputPath, binaryData.data(), binaryData.size());
    }

    // 构建资源信息
    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Texture";
    res.size = static_cast<int64_t>(binaryData.size());
    result.resources.push_back(res);

    return result;
}

std::vector<ConversionResult::ConvertedResource> TextureConverter::convertAll(
    const std::vector<TextureData>& textures,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (size_t i = 0; i < textures.size(); i++) {
        const auto& texData = textures[i];
        std::string name = texData.name.empty()
            ? baseName + "_" + std::to_string(i) + "_" + texData.usage + ".aytex"
            : baseName + "_" + texData.name + texData.usage + ".aytex";

        std::string virtualPath = "textures/" + name;
        std::string fullOutputPath;
        if (!outputDir.empty()) {
            fullOutputPath = outputDir + "/" + virtualPath;
        }

        // 跳过已存在的文件（中断恢复）
        if (!fullOutputPath.empty() && ayt::io::File::exists(fullOutputPath)) {
            UInt8 existingFormat = 0;
            if (getAytexFormat(fullOutputPath, existingFormat)) {
                if (existingFormat == static_cast<UInt8>(texData.format)) {
                    printf("  [SKIP] %s (format matches, skip)\n", name.c_str());
                    continue;
                } else {
                    printf("  [REPLACE] %s (format changed, reconverting)\n", name.c_str());
                }
            }
        }

        // 创建 Texture
        Texture texture;
        texture._width = static_cast<UInt32>(texData.width);
        texture._height = static_cast<UInt32>(texData.height);
        texture._format = texData.format;

        // 计算 mipmap 数量
        UInt32 mipWidth = texture._width;
        UInt32 mipHeight = texture._height;
        UInt32 mipCount = 1;
        while (mipWidth > 1 || mipHeight > 1) {
            mipWidth = ayt::math::max(1u, mipWidth / 2);
            mipHeight = ayt::math::max(1u, mipHeight / 2);
            mipCount++;
        }

        if (!generateMipmaps) {
            mipCount = 1;
        }

        texture._mipmapCount = mipCount;

        // 如果有源数据，生成 mipmap
        if (!texData.imageData.empty()) {
            std::vector<std::vector<UInt8>> mipDatas(mipCount);
            std::vector<UInt32> mipSizes(mipCount);

            mipDatas[0] = texData.imageData;
            mipSizes[0] = static_cast<UInt32>(texData.imageData.size());

            for (UInt32 j = 1; j < mipCount; j++) {
                mipWidth = ayt::math::max(1u, mipWidth / 2);
                mipHeight = ayt::math::max(1u, mipHeight / 2);
                UInt32 mipSize = Texture::computeMipSize(mipWidth, mipHeight, texture._format);
                mipDatas[j].resize(mipSize);
                generateMipmapLevel(mipDatas[j - 1].data(), mipWidth * 2, mipHeight * 2,
                                    texture._format, mipDatas[j].data());
                mipSizes[j] = mipSize;
            }

            // 扁平化存储
            size_t totalSize = 0;
            for (UInt32 j = 0; j < mipCount; j++) {
                totalSize += mipSizes[j];
            }

            texture._imageData.resize(totalSize);
            texture._mipOffsets.resize(mipCount);
            texture._mipSizes.resize(mipCount);

            UInt8* dstPtr = texture._imageData.data();
            for (UInt32 j = 0; j < mipCount; j++) {
                texture._mipOffsets[j] = static_cast<UInt32>(dstPtr - texture._imageData.data());
                texture._mipSizes[j] = mipSizes[j];
                std::memcpy(dstPtr, mipDatas[j].data(), mipSizes[j]);
                dstPtr += mipSizes[j];
            }
        }

        texture._loaded = true;

        // 计算 GUID
        lastGuid = ayt::storage::Guid::computeFromData(texture._imageData.data(), texture._imageData.size());
        texture.setGuid(lastGuid);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        if (!texture.saveToBinary(binaryData)) {
            continue;
        }

        // 写入输出目录（立即写入，支持中断恢复）
        if (!fullOutputPath.empty()) {
            writeFile(fullOutputPath, binaryData.data(), binaryData.size());
        }

        ConversionResult::ConvertedResource res;
        res.guid = lastGuid;
        res.path = virtualPath;
        res.type = "Texture";
        res.size = static_cast<int64_t>(binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource