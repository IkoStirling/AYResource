#pragma once
#include <AYResource/IConverter.h>
#include <utility>

namespace ayt::resource {
/// @brief Optional offline .dlt to .dlc importer; no Session/Scene is retained by AYResource.
/// @note Implementation is owned by AYEntity and linked only when level-resource integration is on.
class DeterministicLevelConverter2D final : public IConverter {
public:
    explicit DeterministicLevelConverter2D(std::string source):source_(std::move(source)) {}
    void setSourcePath(const std::string& path) override {source_=path;}
    void setOutputDir(const std::string& path) override {output_=path;}
    void setLoadOption(LoadOption) override {}
    /// Explicit project source root preserves the complete logical hierarchy and bounds source paths.
    void setAssetRoot(const std::string& path) {root_=path;}
    ConversionResult convert() override;
    bool isValid() const override {return !source_.empty();}
    const char* getSourceType() const override {return "DeterministicLevel2D";}
private:
    std::string source_,output_,root_;
};
} // namespace ayt::resource
