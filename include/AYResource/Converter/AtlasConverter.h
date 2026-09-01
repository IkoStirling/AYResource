#pragma once

#include "AYResource/IConverter.h"

#include <utility>

namespace ayt::resource
{

class AtlasConverter final : public IConverter {
public:
    AtlasConverter() = default;
    explicit AtlasConverter(std::string source) : _sourcePath(std::move(source)) {}

    void setSourcePath(const std::string& path) override { _sourcePath = path; }
    void setOutputDir(const std::string& dir) override { _outputDir = dir; }
    ConversionResult convert() override;
    const char* getSourceType() const override { return "Atlas"; }
    bool isValid() const override { return !_sourcePath.empty(); }
    void setLoadOption(LoadOption option) override { _loadOption = option; }

private:
    std::string _sourcePath;
    std::string _outputDir;
    LoadOption _loadOption = LoadOption::Full;
};

} // namespace ayt::resource
