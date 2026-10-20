#include "AYResource/Converter/TextureConverter.h"
#include "../ImportCacheMetadata.h"
#include "../ImportImage.h"
#include "../ImportDds.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYResource/Converter/TextureCompressor.h"
#include "AYResource/Loader/TextureLoader.h"
#include "AYIO/File.h"
#include "AYMath/MathUtils.h"
#include <AYLog.h>
#include <AYStorage/Guid.h>
#include <cstring>
#include <limits>

// stb_image declarations; implementation lives in Loader/StbImage.cpp so
// runtime loose-texture loading does not depend on this authoring target.
#include <stb_image.h>

namespace ayt::resource
{

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
    if (ayt::io::File::atomicWriteUtf8(path, data, size)) {
        return true;
    }
    ayt::log::warn("[TextureConverter] failed to write %s", path.c_str());
    return false;
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

static bool loadImage(const std::string& path, std::vector<UInt8>& pixels,
                      UInt32& width, UInt32& height, TextureFormat& format) {
    detail::ImportImage image;
    if (!detail::readImportImage(path, image)) return false;
    width = static_cast<UInt32>(image.width);
    height = static_cast<UInt32>(image.height);
    format = TextureFormat::RGBA8;
    pixels = std::move(image.rgba);
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
    return convertFromPath(imagePath, textureName, baseFbxDir, {});
}

ConversionResult TextureConverter::convertFromPath(const std::string& imagePath,
                                               const std::string& sourceTextureName,
                                               const std::string& baseFbxDir,
                                               const std::string& usageSuffixOverride, int sourceChannel) {
    ConversionResult result;
    const std::string textureName = _outputNamespace.empty()
        || sourceTextureName.rfind(_outputNamespace + "/", 0) == 0
        ? sourceTextureName : _outputNamespace + "/" + sourceTextureName;
    const std::string& effectiveUsageSuffix =
        usageSuffixOverride.empty() ? usageSuffix : usageSuffixOverride;

    // 解析完整路径
    std::string fullPath = resolveTexturePath(imagePath, baseFbxDir);
    try { result.sourceFiles.push_back(detail::snapshotImportSource(fullPath)); }
    catch (...) { result.sourceFiles.push_back({fullPath, {}, 0}); return result; }
    std::string ext = getExtension(fullPath);
    for (auto& c : ext) c = static_cast<char>(tolower(c));

    // Dev raw-reference mode: copy source bytes verbatim into textures/
    // with the original extension; .aymat (FBXParser, same stem+suffix)
    // points there and TextureLoader decodes it with stb at runtime.
    // dds/aytex excluded — they flow through passthrough → .aytex below
    // (zero decode, and .dds is not a registered runtime extension).
    if (sourceChannel < -1 || sourceChannel > 3) return result;
    if (rawCopy && sourceChannel < 0 && ext != "dds" && ext != "aytex") {
        const std::string rawFileName = textureName + effectiveUsageSuffix + "." + ext;
        const std::string rawVirtualPath = "textures/" + rawFileName;
        const std::string rawFullOutputPath =
            outputDir.empty() ? std::string() : outputDir + "/" + rawVirtualPath;

        std::vector<UInt8> bytes;
        if (!detail::readImportBytes(fullPath, bytes)) return result;
        const auto srcSize = bytes.size();
        if (!rawFullOutputPath.empty() && !writeFile(rawFullOutputPath, bytes.data(), bytes.size()))
            return result;

        ConversionResult::ConvertedResource res;
        res.path = rawVirtualPath;
        res.type = "Texture";
        res.size = srcSize;
        result.resources.push_back(res);
        return result;
    }

    // 构建输出路径 — 虚拟路径始终为 .aytex（与 aymat 引用契约一致）。
    std::string outputFileName = ayt::asset_format::appendSuffix(
        textureName + effectiveUsageSuffix, ayt::asset_format::Id::Texture);
    std::string virtualPath = "textures/" + outputFileName;
    std::string fullOutputPath;
    if (!outputDir.empty()) {
        fullOutputPath = outputDir + "/" + virtualPath;
    }

    // True passthrough only for already-engine/compressed blobs. PNG/JPG/etc.
    // must decode into a real AYTX header so Texture::load succeeds.
    const bool canRawPassthrough =
        passthrough && (ext == "dds" || ext == "aytex");
    if (canRawPassthrough) {
        if (sourceChannel >= 0) return result; // DDS channel extraction requires a decoder.
        std::vector<UInt8> rawData;
        if (!detail::readImportBytes(fullPath, rawData)) return result;
        Texture texture;
        std::vector<UInt8> binary;
        if (ext == "dds") {
            detail::DdsImportData dds;
            std::string error;
            if (!detail::parseImportDds(rawData, dds, error))
                throw std::runtime_error("DDS import: " + error);
            texture._width=dds.width;texture._height=dds.height;texture._format=dds.format;
            texture.setImageData(std::move(dds.blocks));
            texture.setMipmapLayout(std::move(dds.offsets),std::move(dds.sizes));
            texture.setGuid(ayt::storage::Guid::computeFromData(
                texture.getImageDataBytes(),texture.getImageDataSize()));
            texture.setLoaded(true);
            if(!texture.saveToBinary(binary))return result;
        } else {
            if(!texture.loadFromBinary(rawData.data(),rawData.size()))
                throw std::runtime_error("invalid engine texture passthrough");
            binary=std::move(rawData);
        }
        if (!fullOutputPath.empty() && !writeFile(fullOutputPath,binary.data(),binary.size()))return result;
        ConversionResult::ConvertedResource res;
        res.guid=texture.getGuid();
        res.path=virtualPath;res.type="Texture";res.size=binary.size();
        result.resources.push_back(res);
        return result;
    }

    // 加载源图像
    std::vector<UInt8> pixels;
    UInt32 width = 0;
    UInt32 height = 0;
    TextureFormat format = TextureFormat::RGBA8;

    if (!loadImage(fullPath, pixels, width, height, format)) return result;
    if (sourceChannel >= 0) {
        for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
            const auto scalar = pixels[pixel + std::size_t(sourceChannel)];
            pixels[pixel] = pixels[pixel+1] = pixels[pixel+2] = scalar;
            pixels[pixel+3] = 255;
        }
    }
    TextureData image;
    image.width = int(width); image.height = int(height);
    image.imageData = std::move(pixels);
    auto converted = convertPixels(image, virtualPath);
    converted.sourceFiles = std::move(result.sourceFiles);
    return converted;
}

ConversionResult TextureConverter::convertPixels(const TextureData& image, const std::string& virtualPath)
{
    ConversionResult result;
    if (image.width <= 0 || image.height <= 0 || image.format != TextureFormat::RGBA8
        || image.imageData.size() != std::size_t(image.width) * image.height * 4
        || virtualPath.rfind("textures/", 0) != 0
        || !ayt::io::path::isPortableRelative(virtualPath)) return result;
    if (std::uint64_t(image.width) * image.height * 4 > (std::numeric_limits<UInt32>::max)()) return result;
    const UInt32 width = UInt32(image.width), height = UInt32(image.height);
    const auto& pixels = image.imageData;
    const auto targetFormat = rawCopy ? TextureFormat::RGBA8 : outputFormat;
    // The available encoders are BC3/BC7. Refuse unsupported formats instead
    // of copying RGBA or BC3 bytes into a smaller/differently labelled buffer.
    if (targetFormat != TextureFormat::RGBA8 && targetFormat != TextureFormat::BC3
        && targetFormat != TextureFormat::BC7) return result;
    const std::string fullOutputPath = outputDir.empty() ? std::string{} : outputDir + "/" + virtualPath;
    // 创建 Texture
    Texture texture;
    texture._width = width;
    texture._height = height;
    texture._format = targetFormat;

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
        totalSize += Texture::computeMipSize(mipRGBAWidth[i], mipRGBAHeight[i], targetFormat);
    }

    if (totalSize > (std::numeric_limits<UInt32>::max)()) return result;
    UInt8* dstPtr = texture.mutableImageData(totalSize);
    std::vector<UInt32> mipOffsets(mipCount);
    std::vector<UInt32> mipSizes(mipCount);

    const bool isBC = targetFormat == TextureFormat::BC7 || targetFormat == TextureFormat::BC3
        || targetFormat == TextureFormat::BC1 || targetFormat == TextureFormat::BC5;
    std::unique_ptr<ITextureCompressor> compressor;
    if(isBC) {
        compressor=TextureCompressor::create(targetFormat==TextureFormat::BC7
            ?TextureCompressor::Format::BC7:TextureCompressor::Format::BC3);
        if(!compressor) return result;
        printf("  [INFO] Compressing %ux%u, %u mip levels with %s...\n",width,height,mipCount,compressor->getFormatName());
    }

    for (UInt32 i = 0; i < mipCount; i++) {
        mipOffsets[i] = static_cast<UInt32>(dstPtr - texture.getImageDataBytes());

        if (targetFormat == TextureFormat::BC7 || targetFormat == TextureFormat::BC3 ||
            targetFormat == TextureFormat::BC1 || targetFormat == TextureFormat::BC5) {
            // 压缩格式：使用压缩器
            printf("    Mip[%u/%u]: %ux%u ...\n", i + 1, mipCount, mipRGBAWidth[i], mipRGBAHeight[i]);
            auto compressed = compressor->compress(mipRGBA[i].data(), mipRGBAWidth[i], mipRGBAHeight[i]);
            if (compressed.size() != Texture::computeMipSize(mipRGBAWidth[i], mipRGBAHeight[i], targetFormat))
                return result;
            UInt32 compressedSize = static_cast<UInt32>(compressed.size());
            std::memcpy(dstPtr, compressed.data(), compressedSize);
            mipSizes[i] = compressedSize;
            dstPtr += compressedSize;
        } else {
            // 非压缩格式：直接复制 RGBA 数据
            UInt32 dataSize = mipRGBAWidth[i] * mipRGBAHeight[i] * 4;
            std::memcpy(dstPtr, mipRGBA[i].data(), dataSize);
            mipSizes[i] = dataSize;
            dstPtr += dataSize;
        }
    }

    texture.setMipmapLayout(std::move(mipOffsets), std::move(mipSizes));
    texture.setLoaded(true);

    // 计算 GUID
    lastGuid = ayt::storage::Guid::computeFromData(texture.getImageDataBytes(), texture.getImageDataSize());
    texture.setGuid(lastGuid);

    // 保存到二进制
    std::vector<UInt8> binaryData;
    if (!texture.saveToBinary(binaryData)) {
        return result;
    }

    // 立即写入（中断恢复支持）
    if (!fullOutputPath.empty()
        && !writeFile(fullOutputPath, binaryData.data(), binaryData.size())) {
        return result;
    }

    ConversionResult::ConvertedResource res;
    res.guid = lastGuid;
    res.path = virtualPath;
    res.type = "Texture";
    res.size = static_cast<uint64_t>(binaryData.size());
    result.resources.push_back(res);
    return result;
}

ConversionResult TextureConverter::convert() {
    if (!isValid()) return {};
    return convertFromPath(sourcePath, getBaseName(sourcePath), {}, usageSuffix);
}

std::vector<ConversionResult::ConvertedResource> TextureConverter::convertAll(
    const std::vector<TextureData>& textures,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (size_t i = 0; i < textures.size(); i++) {
        const auto& texData = textures[i];
        std::string name = texData.name.empty()
            ? ayt::asset_format::appendSuffix(
                  baseName + "_" + std::to_string(i) + "_" + texData.usage,
                  ayt::asset_format::Id::Texture)
            : ayt::asset_format::appendSuffix(
                  baseName + "_" + texData.name + texData.usage,
                  ayt::asset_format::Id::Texture);

        std::string virtualPath = "textures/" + name;
        std::string fullOutputPath;
        if (!outputDir.empty()) {
            fullOutputPath = outputDir + "/" + virtualPath;
        }

        const bool rawPixels = texData.format == TextureFormat::RGBA8 || texData.format == TextureFormat::RGB8;
        const bool encodedBlocks = texData.format == TextureFormat::BC1 || texData.format == TextureFormat::BC3
            || texData.format == TextureFormat::BC4 || texData.format == TextureFormat::BC5 || texData.format == TextureFormat::BC7;
        if (texData.width <= 0 || texData.height <= 0 || (!rawPixels && !encodedBlocks)
            || !ayt::io::path::isPortableRelative(virtualPath)) continue;
        const auto w = std::uint64_t(texData.width), h = std::uint64_t(texData.height);
        const auto expectedBytes = rawPixels ? w * h * (texData.format == TextureFormat::RGBA8 ? 4 : 3)
            : ((w+3)/4) * ((h+3)/4)
                * (texData.format == TextureFormat::BC1 || texData.format == TextureFormat::BC4 ? 8 : 16);
        if (expectedBytes > (std::numeric_limits<UInt32>::max)()) continue;
        const auto expected = Texture::computeMipSize(UInt32(texData.width), UInt32(texData.height), texData.format);
        if (!expected || texData.imageData.size() != expected) continue;

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

        if (!generateMipmaps || !rawPixels) {
            // Already encoded blocks carry exactly one complete mip. Never
            // filter them as RGB bytes or invent an absent mip chain.
            mipCount = 1;
        }

        texture._mipmapCount = mipCount;

        // 如果有源数据，生成 mipmap
        if (!texData.imageData.empty()) {
            std::vector<std::vector<UInt8>> mipDatas(mipCount);
            std::vector<UInt32> mipSizes(mipCount);

            mipDatas[0] = texData.imageData;
            mipSizes[0] = static_cast<UInt32>(texData.imageData.size());

            mipWidth = texture._width;
            mipHeight = texture._height;
            for (UInt32 j = 1; j < mipCount; j++) {
                const auto sourceWidth = mipWidth, sourceHeight = mipHeight;
                mipWidth = ayt::math::max(1u, mipWidth / 2);
                mipHeight = ayt::math::max(1u, mipHeight / 2);
                UInt32 mipSize = Texture::computeMipSize(mipWidth, mipHeight, texture._format);
                mipDatas[j].resize(mipSize);
                generateMipmapLevel(mipDatas[j - 1].data(), sourceWidth, sourceHeight,
                                    texture._format, mipDatas[j].data());
                mipSizes[j] = mipSize;
            }

            // 扁平化存储
            size_t totalSize = 0;
            for (UInt32 j = 0; j < mipCount; j++) {
                totalSize += mipSizes[j];
            }

            if (totalSize > (std::numeric_limits<UInt32>::max)()) continue;
            UInt8* dstPtr = texture.mutableImageData(totalSize);
            std::vector<UInt32> mipOffsets(mipCount);
            std::vector<UInt32> mipSizesOut(mipCount);
            for (UInt32 j = 0; j < mipCount; j++) {
                mipOffsets[j] = static_cast<UInt32>(dstPtr - texture.getImageDataBytes());
                mipSizesOut[j] = mipSizes[j];
                std::memcpy(dstPtr, mipDatas[j].data(), mipSizes[j]);
                dstPtr += mipSizes[j];
            }
            texture.setMipmapLayout(std::move(mipOffsets), std::move(mipSizesOut));
        }

        texture.setLoaded(true);

        // 计算 GUID
        lastGuid = ayt::storage::Guid::computeFromData(texture.getImageDataBytes(), texture.getImageDataSize());
        texture.setGuid(lastGuid);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        if (!texture.saveToBinary(binaryData)) {
            continue;
        }

        // 写入输出目录（立即写入，支持中断恢复）
        if (!fullOutputPath.empty()
            && !writeFile(fullOutputPath, binaryData.data(), binaryData.size())) {
            continue;
        }

        ConversionResult::ConvertedResource res;
        res.guid = lastGuid;
        res.path = virtualPath;
        res.type = "Texture";
        res.size = static_cast<uint64_t>(binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource
