#pragma once
#include "AYResource/IConverter.h"
#include "AYResource/assetsImpl/Video.h"
#include "AYResource/IntermediateAsset.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== VideoConverter — 视频格式转换器 =====
// 将 MP4/AVI 等视频格式转换为 .ayvideo 引擎格式
class VideoConverter : public IConverter {
public:
    VideoConverter();
    explicit VideoConverter(const std::string& sourcePath);
    virtual ~VideoConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Video"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

    // ===== 输出选项 =====
    void setOutputWidth(UInt32 width) { outputWidth = width; }
    UInt32 getOutputWidth() const { return outputWidth; }

    void setOutputHeight(UInt32 height) { outputHeight = height; }
    UInt32 getOutputHeight() const { return outputHeight; }

    void setFrameRate(float frameRate) { frameRate = frameRate; }
    float getFrameRate() const { return frameRate; }

    void setMaxFrameCount(UInt32 maxFrames) { maxFrameCount = maxFrames; }
    UInt32 getMaxFrameCount() const { return maxFrameCount; }

    // ===== 批量转换（从 IntermediateAsset） =====
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<VideoData>& videos,
        const std::string& baseName
    );

private:
    std::string sourcePath;
    std::string outputDir;
    LoadOption loadOption = IConverter::LoadOption::MeshOnly;
    UInt32 outputWidth = 1920;
    UInt32 outputHeight = 1080;
    float frameRate = 30.0f;
    UInt32 maxFrameCount = 600;  // 最多 20 秒 @ 30fps
    ayt::math::FGuid lastGuid;

    // 辅助方法
    static bool writeFile(const std::string& path, const void* data, size_t size);
    static std::string getBaseName(const std::string& path);
    static std::string getExtension(const std::string& path);
};

} // namespace ayt::resource