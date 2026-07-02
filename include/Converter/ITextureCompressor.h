#pragma once
#include <cstdint>
#include <vector>
#include <memory>

namespace ayt::resource
{

// ===== ITextureCompressor - 纹理压缩器接口 =====
class ITextureCompressor {
public:
    virtual ~ITextureCompressor() = default;

    // 压缩图像
    // @param rgba RGBA 格式像素数据 (width * height * 4 bytes)
    // @param width 图像宽度
    // @param height 图像高度
    // @return 压缩后的数据
    virtual std::vector<uint8_t> compress(const uint8_t* rgba, int width, int height) = 0;

    // 获取压缩块大小（字节）
    virtual int getBlockSize() const = 0;

    // 获取压缩格式名称
    virtual const char* getFormatName() const = 0;
};

} // namespace ayt::resource