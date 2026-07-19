#include "AYVideo.h"
#include <ayio/File.h>
#include <cstring>
#include <algorithm>

namespace ayt::resource
{

// ===== 常量 =====
static constexpr UInt32 MAX_FRAME_COUNT = 65536;

// ===== Video 文件头 (二进制格式) =====
#pragma pack(push, 1)
struct VideoBinaryHeader {
    UInt32 magic;              // 'AYVD' = 0x44415941
    UInt16 version;            // 版本 = 1
    FGuid guid;                // 资源唯一标识 (16 bytes)
    UInt16 nameLength;         // 名称长度
    UInt32 width;              // 视频宽度
    UInt32 height;             // 视频高度
    float duration;            // 持续时间 (秒)
    float frameRate;           // 帧率
    UInt32 frameCount;         // 帧数量
    UInt32 frameSize;          // 每帧字节大小
    UInt32 frameDataSize;      // 帧数据总大小
};
#pragma pack(pop)

// ===== Video =====

Video::Video() = default;

void Video::clear() {
    _frameData.clear();
    _name.clear();
    _width = 0;
    _height = 0;
    _duration = 0.0f;
    _frameRate = 30.0f;
    _frameCount = 0;
    _frameSize = 0;
}

bool Video::unload() {
    clear();
    _loaded = false;
    return true;
}

size_t Video::sizeInBytes() const {
    return sizeof(Video) + _frameData.size();
}

bool Video::load(const std::string& path) {
    _path = path;

    ayt::io::File file(_path, ayt::io::File::Mode::BinaryRead);
    if (!file.isOpen()) {
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize < sizeof(VideoBinaryHeader)) {
        return false;
    }

    std::vector<UInt8> data(fileSize);
    if (file.read(data.data(), fileSize) != fileSize) {
        return false;
    }

    return loadFromBinary(data.data(), data.size());
}

bool Video::loadFromBinary(const void* data, size_t size) {
    if (!data || size < sizeof(VideoBinaryHeader)) {
        return false;
    }

    clear();

    const UInt8* ptr = static_cast<const UInt8*>(data);
    const VideoBinaryHeader* header = reinterpret_cast<const VideoBinaryHeader*>(ptr);

    // 验证 magic 和 version
    if (header->magic != IVideo::MAGIC || header->version != IVideo::VERSION) {
        return false;
    }

    // 读取 GUID
    _guid = header->guid;

    // 读取基本属性
    _width = header->width;
    _height = header->height;
    _duration = header->duration;
    _frameRate = header->frameRate;
    _frameCount = header->frameCount;
    _frameSize = header->frameSize;

    // 读取 name
    if (header->nameLength > 0) {
        if (size < sizeof(VideoBinaryHeader) + header->nameLength) {
            return false;
        }
        _name.assign(reinterpret_cast<const char*>(ptr + sizeof(VideoBinaryHeader)), header->nameLength);
    } else {
        _name.clear();
    }

    // 读取帧数据
    size_t dataOffset = sizeof(VideoBinaryHeader) + header->nameLength;
    if (header->frameDataSize > 0) {
        if (size < dataOffset + header->frameDataSize) {
            return false;
        }
        _frameData.resize(header->frameDataSize);
        std::memcpy(_frameData.data(), ptr + dataOffset, header->frameDataSize);
    }

    _loaded = true;
    return true;
}

bool Video::saveToBinary(std::vector<UInt8>& outData) const {
    if (_frameCount == 0 || _frameSize == 0) {
        return false;
    }

    // 计算总大小
    size_t nameLen = _name.size();
    size_t totalSize = sizeof(VideoBinaryHeader) + nameLen + _frameData.size();

    outData.resize(totalSize);
    UInt8* ptr = outData.data();

    // 写入 Custom Header
    VideoBinaryHeader header;
    std::memset(&header, 0, sizeof(header));
    header.magic = IVideo::MAGIC;
    header.version = IVideo::VERSION;
    header.guid = _guid;
    header.nameLength = static_cast<UInt16>(nameLen);
    header.width = _width;
    header.height = _height;
    header.duration = _duration;
    header.frameRate = _frameRate;
    header.frameCount = _frameCount;
    header.frameSize = _frameSize;
    header.frameDataSize = static_cast<UInt32>(_frameData.size());

    std::memcpy(ptr, &header, sizeof(header));

    // 写入 name
    if (nameLen > 0) {
        std::memcpy(ptr + sizeof(VideoBinaryHeader), _name.data(), nameLen);
    }

    // 写入帧数据
    if (!_frameData.empty()) {
        std::memcpy(ptr + sizeof(VideoBinaryHeader) + nameLen, _frameData.data(), _frameData.size());
    }

    return true;
}

void Video::createTestVideo(UInt32 width, UInt32 height, UInt32 frameCount, float frameRate) {
    clear();

    _width = width;
    _height = height;
    _frameCount = frameCount;
    _frameRate = frameRate;
    _duration = static_cast<float>(frameCount) / frameRate;
    _frameSize = width * height * 4;  // RGBA

    // 创建测试帧数据 (简单的颜色渐变)
    _frameData.resize(static_cast<size_t>(_frameCount) * _frameSize);

    for (UInt32 f = 0; f < frameCount; f++) {
        UInt8* framePtr = _frameData.data() + static_cast<size_t>(f) * _frameSize;
        float t = static_cast<float>(f) / static_cast<float>(frameCount);

        for (UInt32 y = 0; y < height; y++) {
            for (UInt32 x = 0; x < width; x++) {
                UInt32 pixelIndex = (y * width + x) * 4;
                float px = static_cast<float>(x) / static_cast<float>(width);
                float py = static_cast<float>(y) / static_cast<float>(height);

                // RGB 颜色基于位置和时间
                framePtr[pixelIndex + 0] = static_cast<UInt8>(255.0f * px * t);        // R
                framePtr[pixelIndex + 1] = static_cast<UInt8>(255.0f * py * (1.0f - t)); // G
                framePtr[pixelIndex + 2] = static_cast<UInt8>(255.0f * t);            // B
                framePtr[pixelIndex + 3] = 255;                                       // A
            }
        }
    }

    _loaded = true;
}

} // namespace ayt::resource