#pragma once
#include "ITextureCompressor.h"
#include <memory>

namespace ayt::resource
{

// ===== TextureCompressor - 纹理压缩器工厂 =====
class TextureCompressor {
public:
    enum class Format {
        BC3,   // DXT5, 16 bytes/block, RGBA (当前 squish 实现)
        BC7,   // 更高质量 RGBA (Basis Universal 支持)
    };

    // 创建压缩器
    static std::unique_ptr<ITextureCompressor> create(Format format);

    // 获取格式名称
    static const char* getFormatName(Format format);
};

} // namespace ayt::resource