#include "Converter\TextureCompressor.h"
#include "Converter\ITextureCompressor.h"
#include <squish.h>
#include <basisu/encoder/basisu_comp.h>
#include <basisu/transcoder/basisu_transcoder.h>
#include <cstring>

namespace ayt::resource
{

// ===== SquishCompressor - squish 库实现 =====
class SquishCompressor : public ITextureCompressor {
public:
    SquishCompressor(int flags) : _flags(flags) {}

    std::vector<uint8_t> compress(const uint8_t* rgba, int width, int height) override {
        int blocksWide = (width + 3) / 4;
        int blocksHigh = (height + 3) / 4;
        int compressedSize = blocksWide * blocksHigh * 16;

        std::vector<uint8_t> result(compressedSize);
        squish::CompressImage(rgba, width, height, result.data(), _flags);
        return result;
    }

    int getBlockSize() const override { return 16; }

    const char* getFormatName() const override { return "BC3 (DXT5)"; }

private:
    int _flags;
};

// ===== BasisCompressor - Basis Universal 实现 =====
// 两步：1. 压缩到 .basis (UASTC)  2. 转码到 BC7
class BasisCompressor : public ITextureCompressor {
public:
    BasisCompressor() {
        // 初始化 Basis Universal 编码器
        basisu::basisu_encoder_init();
        // 初始化转码器
        basist::basisu_transcoder_init();
    }

    std::vector<uint8_t> compress(const uint8_t* rgba, int width, int height) override {
        // Step 1: 压缩到 .basis 格式 (UASTC 4x4)
        // quality_level: 0-100 (越高质量越好)
        // effort_level: 0-10 (越高越慢)
        size_t basisSize = 0;
        void* basisData = basisu::basis_compress2(
            basist::basis_tex_format::cUASTC_LDR_4x4,
            rgba, width, height, width,  // pitch = width for RGBA
            0,  // flags
            80, // quality_level (high)
            5,  // effort_level (balanced)
            &basisSize
        );

        if (!basisData || basisSize == 0) {
            return {};
        }

        // Step 2: 转码到 BC7
        std::vector<uint8_t> bc7Result;

        // 计算 BC7 输出大小
        int blocksWide = (width + 3) / 4;
        int blocksHigh = (height + 3) / 4;
        uint32_t bc7Size = blocksWide * blocksHigh * 16;
        bc7Result.resize(bc7Size);

        // 创建转码器
        basist::basisu_transcoder transcoder;
        if (!transcoder.start_transcoding(basisData, (uint32_t)basisSize)) {
            basisu::basis_free_data(basisData);
            return {};
        }

        // 转码第一个 mip level 到 BC7
        if (!transcoder.transcode_image_level(
                basisData, (uint32_t)basisSize,
                0, 0,  // image_index, level_index
                bc7Result.data(),
                blocksWide * blocksHigh,
                basist::transcoder_texture_format::cTFBC7_RGBA)) {
            transcoder.stop_transcoding();
            basisu::basis_free_data(basisData);
            return {};
        }

        transcoder.stop_transcoding();
        basisu::basis_free_data(basisData);

        return bc7Result;
    }

    int getBlockSize() const override { return 16; }

    const char* getFormatName() const override { return "BC7 (Basis)"; }
};

// ===== TextureCompressor 工厂 =====
std::unique_ptr<ITextureCompressor> TextureCompressor::create(Format format) {
    switch (format) {
        case Format::BC3:
            return std::make_unique<SquishCompressor>(squish::kDxt5 | squish::kColourIterativeClusterFit);
        case Format::BC7:
            return std::make_unique<BasisCompressor>();
        default:
            return std::make_unique<SquishCompressor>(squish::kDxt5 | squish::kColourIterativeClusterFit);
    }
}

const char* TextureCompressor::getFormatName(Format format) {
    switch (format) {
        case Format::BC3: return "BC3 (DXT5)";
        case Format::BC7: return "BC7 (Basis)";
        default: return "Unknown";
    }
}

} // namespace ayt::resource