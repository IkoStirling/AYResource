#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <AYAssetFormat/AssetFormat.h>
#include <AYIO/File.h>
#include <AYResource/AudioContent.h>
#include <AYResource/Converter/AudioContentConverter.h>
#include <AYResource/authoring/PerformanceRenderer.h>
#include <AYStorage/Guid.h>
#include <set>
#ifdef _WIN32
#include <Windows.h>
#endif
namespace ayt::resource {
namespace {
namespace fs = std::filesystem;
using namespace ayt::audio;
fs::path builtinRoot() {
  // Offline tool input only. No reference to this directory enters a cooked
  // bundle.
  fs::path executable;
#ifdef _WIN32
  wchar_t buffer[32768]{};
  const auto length = GetModuleFileNameW(nullptr, buffer, 32768);
  if (length && length < 32768)
    executable = fs::path(buffer).parent_path();
#elif defined(__linux__)
  std::error_code error;
  executable = fs::read_symlink("/proc/self/exe", error).parent_path();
#endif
  if (!executable.empty())
    for (const auto &candidate :
         {executable / "assets", executable / "assets/AYAudio"})
      if (fs::is_regular_file(candidate / "samples/C1.aud"))
        return candidate;
#ifdef AY_AUDIO_CONTENT_BUILTIN_ROOT
  return fs::u8path(AY_AUDIO_CONTENT_BUILTIN_ROOT);
#else
  return {};
#endif
}
bool sourceType(const fs::path &path, asset_format::Id id) {
  return asset_format::matchesPath(chart::utf8(path), id);
}
fs::path assetRoot(const fs::path &source, const fs::path &requested) {
  if (!requested.empty())
    return fs::canonical(requested);
  for (auto p = source.parent_path(); !p.empty() && p != p.parent_path();
       p = p.parent_path())
    if (p.filename() == "Assets")
      return fs::canonical(p);
  return fs::canonical(source.parent_path());
}
fs::path reference(const fs::path &root, const std::string &value,
                   bool relative) {
  if (!relative)
    throw std::runtime_error(
        "publishable audio references must be relative to project Assets");
  const auto file = chart::containedPath(chart::utf8(root), value);
  if (!fs::is_regular_file(file))
    throw std::runtime_error("missing audio dependency: " + value);
  return file;
}
} // namespace
AudioContentSource inspectAudioContentSource(const fs::path &source,
                                             const fs::path &requested) {
  AudioContentSource result;
  result.source = fs::canonical(source);
  result.root = assetRoot(result.source, requested);
  const auto logical = fs::relative(result.source, result.root);
  if (chart::containedPath(chart::utf8(result.root), chart::utf8(logical)) !=
      result.source)
    throw std::runtime_error("audio content source escapes Assets");
  const auto j = nlohmann::json::parse(chart::read(result.source));
  std::set<fs::path> dependencies;
  result.chart = sourceType(source, asset_format::Id::AudioChartSource);
  if (result.chart) {
    const auto doc = chart::Document::fromJson(j);
    dependencies.insert(reference(result.root, doc.audio, doc.audioRelative));
  } else if (sourceType(source, asset_format::Id::AudioPerformanceSource)) {
    const auto doc = performance::Document::fromJson(j);
    bool piano = false;
    for (const auto &track : doc.tracks)
      for (const auto &clip : track.clips) {
        const auto &i = clip.instrument;
        if (i.kind == "sample")
          dependencies.insert(reference(result.root, i.source, i.relative));
        if (i.kind == "piano")
          piano = true;
      }
    if (piano) {
      const auto samples = builtinRoot() / "samples";
      for (int octave = 1; octave <= 7; ++octave) {
        const auto sample = samples / ("C" + std::to_string(octave) + ".aud");
        if (!fs::is_regular_file(sample))
          throw std::runtime_error("builtin piano cook samples unavailable; "
                                   "use imported project samples or sine");
        dependencies.insert(fs::canonical(sample));
      }
    }
  } else
    throw std::runtime_error("unsupported audio authoring source");
  result.dependencies.assign(dependencies.begin(), dependencies.end());
  return result;
}
ConversionResult AudioContentConverter::convert() {
  const auto source =
      inspectAudioContentSource(fs::u8path(_source), fs::u8path(_root));
  const auto j = nlohmann::json::parse(chart::read(source.source));
  Audio audio;
  std::vector<AudioContentEvent> events;
  double bpm = 120, offset = 0;
  if (source.chart) {
    const auto doc = chart::Document::fromJson(j);
    bpm = doc.bpm;
    offset = doc.offset;
    auto sample =
        performance::decodeSample(chart::utf8(source.dependencies.front()));
    const double duration =
        double(sample.pcm.size() / sample.channels) / sample.rate;
    doc.validate(duration);
    if (duration > 600 || sample.pcm.size() * sizeof(float) > 256 * 1024 * 1024)
      throw std::runtime_error("chart music exceeds runtime PCM budget");
    audio.setFormat(sample.rate, sample.channels, 32,
                    sample.pcm.size() / sample.channels);
    audio.setData(reinterpret_cast<const uint8_t *>(sample.pcm.data()),
                  sample.pcm.size() * sizeof(float));
    for (const auto &n : doc.notes)
      events.push_back({uint64_t(std::llround(n.time * sample.rate)),
                        uint64_t(std::llround(n.end * sample.rate)),
                        uint32_t(n.lane), 0, 1});
  } else {
    const auto doc = performance::Document::fromJson(j);
    bpm = doc.bpm;
    performance::RenderOptions options;
    options.assets = chart::utf8(source.root);
    options.builtinAssets = chart::utf8(builtinRoot());
    const auto rendered = performance::render(doc, options);
    audio.setFormat(rendered.rate, 2, 32, rendered.pcm.size() / 2);
    audio.setData(reinterpret_cast<const uint8_t *>(rendered.pcm.data()),
                  rendered.pcm.size() * sizeof(float));
    const bool solo = std::any_of(doc.tracks.begin(), doc.tracks.end(),
                                  [](const auto &t) { return t.solo; });
    for (size_t channel = 0; channel < doc.tracks.size(); ++channel) {
      const auto &track = doc.tracks[channel];
      if (track.mute || (solo && !track.solo))
        continue;
      for (const auto &clip : track.clips)
        for (int repeat = 0; repeat < clip.repeats; ++repeat) {
          const auto left = clip.position + clip.length * repeat,
                     base = left - clip.offset, right = left + clip.length;
          for (const auto &n : clip.notes) {
            const auto begin = std::max(left, base + n.start),
                       end = std::min(right,
                                      base + performance::releaseTick(clip, n));
            if (begin >= end)
              continue;
            events.push_back(
                {uint64_t(std::llround(doc.seconds(begin) * rendered.rate)),
                 uint64_t(std::llround(doc.seconds(end) * rendered.rate)),
                 uint32_t(channel), uint8_t(n.pitch), n.velocity});
            if (events.size() > 100000)
              throw std::runtime_error("compiled audio event budget exceeded");
          }
        }
    }
  }
  std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
    return a.beginFrame == b.beginFrame ? a.channel < b.channel
                                        : a.beginFrame < b.beginFrame;
  });
  std::string credits;
  if (!source.chart) {
    const auto doc = performance::Document::fromJson(j);
    for (const auto &track : doc.tracks)
      for (const auto &clip : track.clips)
        if (clip.instrument.kind == "piano")
          credits =
              "Salamander Grand Piano by Alexander Holm. CC BY 3.0: "
              "https://creativecommons.org/licenses/by/3.0/\nSource subset: "
              "https://github.com/Tonejs/audio/tree/master/"
              "salamander\nModified by sample selection, pitch transposition "
              "and arrangement rendering.\n";
  }
  AudioContent content;
  audio.setName("compiled-audio");
  audio.setLoaded(true);
  if (!content.assign(audio,
                      source.chart ? AudioContent::Kind::Chart
                                   : AudioContent::Kind::Performance,
                      bpm, offset, std::move(events), std::move(credits)))
    throw std::runtime_error("invalid compiled audio content");
  std::vector<uint8_t> bytes;
  if (!content.encode(bytes))
    throw std::runtime_error("cannot encode audio content");
  const auto sourceMatch = asset_format::matchPath(_source);
  const auto portable =
      fs::relative(source.source, source.root).generic_u8string();
  std::string relative(portable.begin(), portable.end());
  relative.resize(relative.size() - sourceMatch->suffix.size());
  const auto logical =
      asset_format::appendSuffix(relative, asset_format::Id::AudioContent);
  const auto file = chart::containedPath(_output, logical);
  fs::create_directories(file.parent_path());
  if (!io::File::atomicWrite(chart::utf8(file), bytes.data(), bytes.size()))
    throw std::runtime_error("cannot write audio content");
  ConversionResult result;
  result.importerContractTag = "audio-content-bundle-v1";
  result.resources.push_back(
      {storage::Guid::computeFromData(bytes.data(), bytes.size()),
       logical,
       "AudioContent",
       {},
       uint64_t(bytes.size())});
  // Dependencies are embedded; the bundle has no runtime reference to source
  // files.
  return result;
}
} // namespace ayt::resource
