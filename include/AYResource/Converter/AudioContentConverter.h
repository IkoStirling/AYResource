#pragma once
#include <AYResource/IConverter.h>
#include <filesystem>
namespace ayt::resource {
struct AudioContentSource {
  std::filesystem::path source, root;
  std::vector<std::filesystem::path> dependencies;
  bool chart = false;
};
/// Validate authoring schemas and discover every external PCM dependency before
/// Cook. Absolute/outside-Assets user references are rejected; shipped piano
/// samples are separately hashed.
AudioContentSource
inspectAudioContentSource(const std::filesystem::path &source,
                          const std::filesystem::path &assetRoot);
class AudioContentConverter final : public IConverter {
public:
  explicit AudioContentConverter(std::string source)
      : _source(std::move(source)) {}
  void setSourcePath(const std::string &path) override { _source = path; }
  void setOutputDir(const std::string &path) override { _output = path; }
  void setLoadOption(LoadOption) override {}
  void setAssetRoot(const std::string &path) { _root = path; }
  ConversionResult convert() override;
  bool isValid() const override { return !_source.empty(); }
  const char *getSourceType() const override { return "AudioContent"; }

private:
  std::string _source, _output, _root;
};
} // namespace ayt::resource
