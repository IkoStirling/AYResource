#pragma once
#include "IAYConverter.h"
#include "AYAudio.h"
#include "AYIntermediateAsset.h"
#include <memory>
#include <string>
#include <vector>

namespace ayt::resource
{

// ===== AudioConverter — 音频格式转换器 =====
// 将音频格式（WAV/OGG）转换为 .ayaudio 引擎格式
class AudioConverter : public IConverter {
public:
    AudioConverter();
    explicit AudioConverter(const std::string& sourcePath);
    virtual ~AudioConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Audio"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

    // ===== 输出格式选项 =====
    void setOutputSampleRate(UInt32 sampleRate) { outputSampleRate = sampleRate; }
    UInt32 getOutputSampleRate() const { return outputSampleRate; }

    void setOutputChannels(UInt32 channels) { outputChannels = channels; }
    UInt32 getOutputChannels() const { return outputChannels; }

    void setOutputBitsPerSample(UInt32 bits) { outputBitsPerSample = bits; }
    UInt32 getOutputBitsPerSample() const { return outputBitsPerSample; }

    // ===== 批量转换（从 IntermediateAsset） =====
    std::vector<ConversionResult::ConvertedResource> convertAll(
        const std::vector<AudioData>& audios,
        const std::string& baseName
    );

private:
    std::string sourcePath;
    std::string outputDir;
    LoadOption loadOption = IConverter::LoadOption::Full;
    UInt32 outputSampleRate = 44100;
    UInt32 outputChannels = 2;
    UInt32 outputBitsPerSample = 16;
    ayt::math::FGuid lastGuid;
};

} // namespace ayt::resource