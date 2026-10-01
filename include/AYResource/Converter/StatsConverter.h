#pragma once
#include <AYResource/IConverter.h>

namespace ayt::resource {
class StatsConverter final : public IConverter {
public:
    explicit StatsConverter(std::string source) : _source(std::move(source)) {}
    void setSourcePath(const std::string& path) override { _source=path; }
    void setOutputDir(const std::string& path) override { _output=path; }
    void setLoadOption(LoadOption) override {}
    void setAssetRoot(const std::string& path) { _root=path; }
    ConversionResult convert() override;
    bool isValid() const override { return !_source.empty(); }
    const char* getSourceType() const override { return "StatsRecipes"; }
private:
    std::string _source, _output, _root;
};
}
