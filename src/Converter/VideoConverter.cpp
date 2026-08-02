#include "Converter\VideoConverter.h"
#include "Loader\VideoLoader.h"
#include <ayio/File.h>
#include <aystorage/Guid.h>
#include <fstream>
#include <cstring>

namespace ayt::resource
{

VideoConverter::VideoConverter() = default;

VideoConverter::VideoConverter(const std::string& sourcePath)
    : sourcePath(sourcePath) {}

void VideoConverter::setSourcePath(const std::string& path) {
    sourcePath = path;
}

void VideoConverter::setOutputDir(const std::string& dir) {
    outputDir = dir;
}

// ===== 工具函数 =====

bool VideoConverter::writeFile(const std::string& path, const void* data, size_t size) {
    return ayt::io::File::atomicWrite(path, data, size);
}

std::string VideoConverter::getBaseName(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    std::string fileName = (pos == std::string::npos) ? path : path.substr(pos + 1);
    size_t dotPos = fileName.find_last_of('.');
    if (dotPos == std::string::npos) {
        return fileName;
    }
    return fileName.substr(0, dotPos);
}

std::string VideoConverter::getExtension(const std::string& path) {
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) {
        return "";
    }
    return path.substr(dotPos + 1);
}

// ===== 转换逻辑 =====
// 注意: 完整的 MP4/AVI 解码需要视频解码库 (如 ffmpeg)
// 这里提供简化版本, 支持从 VideoData 直接转换

ConversionResult VideoConverter::convert() {
    ConversionResult result;

    if (!isValid()) {
        return result;
    }

    std::string ext = getExtension(sourcePath);
    for (auto& c : ext) c = static_cast<char>(tolower(c));

    // 检查是否支持该格式
    if (ext != "mp4" && ext != "avi" && ext != "mov" && ext != "mkv") {
        // 如果不是标准视频格式, 尝试作为原始数据处理
        // 这里简化处理: 假设是无效的源
        return result;
    }

    // 实际实现需要集成视频解码库 (如 FFmpeg)
    // 步骤:
    // 1. 打开视频文件
    // 2. 读取帧数据 (解码为 RGBA)
    // 3. 可选: 缩放到目标分辨率
    // 4. 创建 Video 对象并序列化

    // 目前返回空结果, 表示需要外部视频解码库
    return result;
}

std::vector<ConversionResult::ConvertedResource> VideoConverter::convertAll(
    const std::vector<VideoData>& videos,
    const std::string& baseName
) {
    std::vector<ConversionResult::ConvertedResource> results;

    for (size_t i = 0; i < videos.size(); i++) {
        const auto& videoData = videos[i];

        // 创建 Video
        Video video;
        video.setName(videoData.name.empty()
            ? baseName + "_" + std::to_string(i)
            : videoData.name);
        const UInt32 w = static_cast<UInt32>(videoData.width);
        const UInt32 h = static_cast<UInt32>(videoData.height);
        const UInt32 frameSize = w * h * 4;
        const UInt32 frameCount = static_cast<UInt32>(
            videoData.frameData.size() / (frameSize > 0 ? frameSize : 1));
        video.setFrameInfo(w, h, videoData.frameRate, frameCount, frameSize);
        video.setFrameData(videoData.frameData);
        video.setLoaded(true);

        // 计算 GUID
        lastGuid = ayt::storage::Guid::computeFromData(video.getFrameDataBytes(), video.getFrameDataBytesSize());
        video.setGuid(lastGuid);

        // 保存到二进制
        std::vector<UInt8> binaryData;
        if (!video.saveToBinary(binaryData)) {
            continue;
        }

        // 生成虚拟路径: video/{name}.ayvideo
        std::string outputName = video.getName() + std::string(".ayvideo");
        std::string virtualPath = "video/" + outputName;

        // 写入输出目录
        if (!outputDir.empty()) {
            std::string fullPath = outputDir + "/" + virtualPath;
            if (!ayt::io::File::exists(fullPath)) {
                writeFile(fullPath, binaryData.data(), binaryData.size());
            }
        }

        ConversionResult::ConvertedResource res;
        res.guid = lastGuid;
        res.path = virtualPath;
        res.type = "Video";
        res.size = static_cast<uint64_t>(binaryData.size());
        results.push_back(res);
    }

    return results;
}

} // namespace ayt::resource