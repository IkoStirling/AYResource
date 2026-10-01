#pragma once
#include <AYResource/IConverter.h>
namespace ayt::resource {
/// Offline adapter: validated .ayparticle + texture closure, preserving the
/// effect's Assets-relative path. Release cook emits RGBA8 .aytex + mipmaps.
class ParticleConverter final : public IConverter {
public:
    explicit ParticleConverter(std::string source) : _source(std::move(source)) {}
    void setSourcePath(const std::string& path) override { _source=path; }
    void setOutputDir(const std::string& path) override { _output=path; }
    void setLoadOption(LoadOption) override {}
    void setCookTextures(bool value) override { _cook=value; }
    void setAssetRoot(const std::string& path) { _root=path; }
    ConversionResult convert() override;
    bool isValid() const override { return !_source.empty(); }
    const char* getSourceType() const override { return "ParticleEffect"; }
private:
    std::string _source, _output, _root;
    bool _cook=true;
};
}
