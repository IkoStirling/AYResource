#pragma once
#include <AYResource/IResourceLoader.h>
#include <AYResource/assetsImpl/Audio.h>
namespace ayt::resource {
struct AudioContentEvent {
  uint64_t beginFrame = 0, endFrame = 0;
  uint32_t channel = 0;
  uint8_t note = 0;
  float velocity = 1;
};
/// Self-contained compiled arrangement or timed chart: PCM plus immutable event
/// metadata. ResourceManager owns loading/cache/Pak access. No source paths or
/// authoring decoder needed at runtime.
class AudioContent final : public Audio {
public:
  enum class Kind { Performance, Chart };
  bool load(const std::string &path) override;
  bool unload() override;
  size_t sizeInBytes() const override;
  /// Decode .acb v1, <=16 MiB metadata, <=256 MiB PCM, <=100000 events and
  /// <=600 seconds.
  bool decode(const void *bytes, size_t size);
  /// Offline creation; copies validated S16/F32 PCM and validates sorted event
  /// bounds.
  bool assign(const Audio &audio, Kind kind, double bpm, double offset,
              std::vector<AudioContentEvent> events, std::string credits = {});
  bool encode(std::vector<uint8_t> &bytes) const;
  Kind kind() const { return _kind; }
  double bpm() const { return _bpm; }
  double beatOffsetSeconds() const { return _offset; }
  const std::vector<AudioContentEvent> &events() const { return _events; }
  /// Attribution retained from built-in cooked sources; ProjectBuild also
  /// publishes readable notices.
  const std::string &credits() const { return _credits; }

private:
  Kind _kind = Kind::Performance;
  double _bpm = 120, _offset = 0;
  std::vector<AudioContentEvent> _events;
  std::string _credits;
};
class AudioContentLoader final : public IResourceLoader {
public:
  bool canLoad(const std::string &path) const override;
  const char *getResourceType() const override { return "AudioContent"; }
  std::shared_ptr<IResource> load(const std::string &path) override;
  std::shared_ptr<IResource> loadFromBinary(const void *bytes,
                                            size_t size) override;
  std::shared_ptr<IResource> loadAsync(
      const std::string &path,
      std::function<void(std::shared_ptr<IResource>)> callback = {}) override;
};
} // namespace ayt::resource
