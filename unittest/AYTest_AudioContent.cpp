#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <AYIO/File.h>
#include <AYResource/AudioContent.h>
#include <AYResource/Converter/AudioContentConverter.h>
#include <AYResource/ProjectBuild.h>
#include <AYResource/ResourceBootstrap.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/authoring/ChartDocument.h>
#include <AYResource/authoring/PerformanceDocument.h>
#include <AYTest.h>
#include <filesystem>
#include <fstream>
#include <limits>
using namespace ayt::resource;
namespace {
namespace fs = std::filesystem;
Audio pcm() {
  Audio a;
  a.setFormat(48000, 1, 32, 48000);
  std::vector<float> samples(48000, .1f);
  a.setData(reinterpret_cast<const uint8_t *>(samples.data()),
            samples.size() * 4);
  a.setLoaded(true);
  return a;
}
void write(const fs::path &path, const std::vector<uint8_t> &bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  if (!out)
    throw std::runtime_error("test write failed");
}
void text(const fs::path &path, const std::string &value) {
  write(path, {value.begin(), value.end()});
}
ayt::audio::performance::Document performance() {
  using namespace ayt::audio::performance;
  Document d;
  auto &track = d.addTrack();
  Clip clip;
  clip.id = d.allocate();
  clip.name = "sine";
  clip.length = clip.sourceLength = PPQ;
  clip.repeats = 2;
  clip.instrument.kind = "sine";
  Note note;
  note.id = d.allocate();
  note.duration = PPQ / 2;
  clip.notes.push_back(note);
  track.clips.push_back(clip);
  d.validate();
  return d;
}
} // namespace
TEST_SUITE(AudioContentTests)
TEST_CASE(
    BundleRoundTripRejectsTruncationOverflowAndInvalidEventsWithoutReplacingContent) {
  AudioContent a;
  CHECK(a.assign(pcm(), AudioContent::Kind::Chart, 120, .1,
                 {{0, 480, 1, 0, 1}, {960, 1440, 2, 0, .8f}}));
  std::vector<uint8_t> bytes;
  CHECK(a.encode(bytes));
  AudioContent b;
  CHECK(b.decode(bytes.data(), bytes.size()));
  CHECK(b.events().size() == 2);
  CHECK(b.kind() == AudioContent::Kind::Chart);
  CHECK(b.getDataSize() == 192000);
  CHECK(!b.decode(bytes.data(), bytes.size() - 1));
  auto broken = bytes;
  broken[8] = broken[9] = broken[10] = broken[11] = 255;
  CHECK(!b.decode(broken.data(), broken.size()));
  CHECK(b.events().size() == 2);
  CHECK(!b.assign(pcm(), AudioContent::Kind::Chart, 120, 0,
                  {{0, 50000, 0, 0, 1}}));
  CHECK(!b.assign(pcm(), AudioContent::Kind::Chart, 120, 0, {{1, 2, 4, 0, 1}}));
  CHECK(!b.assign(pcm(), AudioContent::Kind::Chart,
                  std::numeric_limits<double>::quiet_NaN(), 0, {}));
}
TEST_CASE(PerformanceAndChartCookEmbedPcmAndDropAuthoringPaths) {
  const auto root = ayt::test::testTmpDir() / "content-source",
             assets = root / "Assets", output = root / "Cooked";
  const auto source = assets / "audio/piece.performance.json";
  text(source, performance().json().dump());
  AudioContentConverter converter(source.string());
  converter.setAssetRoot(assets.string());
  converter.setOutputDir(output.string());
  const auto result = converter.convert();
  CHECK(result.resources.size() == 1);
  CHECK(result.resources[0].path == "audio/piece.acb");
  AudioContent content;
  CHECK(content.load((output / result.resources[0].path).string()));
  CHECK(content.events().size() == 2);
  CHECK(content.getDuration() > 1);
  CHECK(content.getDuration() < 2);
  auto piano = performance();
  piano.tracks[0].clips[0].instrument.kind = "piano";
  text(source, piano.json().dump());
  converter.convert();
  CHECK(content.load((output / "audio/piece.acb").string()));
  CHECK(content.credits().find("Alexander Holm") != std::string::npos);
  std::vector<uint8_t> encoded;
  CHECK(pcm().saveToBinary(encoded));
  write(assets / "audio/music.aud", encoded);
  ayt::audio::chart::Document chart;
  chart.audio = "audio/music.aud";
  chart.audioRelative = true;
  chart.notes = {{1, .1, .3}};
  const auto chartFile = assets / "audio/chart.chart.json";
  text(chartFile, chart.json().dump());
  converter.setSourcePath(chartFile.string());
  const auto chartResult = converter.convert();
  CHECK(chartResult.resources[0].path == "audio/chart.acb");
  CHECK(content.load((output / "audio/chart.acb").string()));
  CHECK(content.kind() == AudioContent::Kind::Chart);
  CHECK(content.events()[0].beginFrame == 4800);
  CHECK(content.events()[0].endFrame == 14400);
  chart.audio = "../escape.aud";
  text(chartFile, chart.json().dump());
  bool rejected = false;
  try {
    converter.convert();
  } catch (...) {
    rejected = true;
  }
  CHECK(rejected);
}
TEST_CASE(
    ProjectCookCacheInvalidatesDependenciesAndPakLoadsWithoutAuthoringTree) {
  const auto root = ayt::test::testTmpDir() / "published-content",
             assets = root / "Assets";
  std::vector<uint8_t> music;
  CHECK(pcm().saveToBinary(music));
  write(assets / "audio/music.aud", music);
  ayt::audio::chart::Document chart;
  chart.audio = "audio/music.aud";
  chart.audioRelative = true;
  chart.notes = {{0, 0, .2}, {3, .5, .5}};
  text(assets / "audio/song.chart.json", chart.json().dump());
  auto piano = performance();
  piano.tracks[0].clips[0].instrument.kind = "piano";
  text(assets / "audio/piano.performance.json", piano.json().dump());
  ProjectBuildProfile profile;
  profile.schemaVersion = 1;
  profile.id = "audio-content";
  profile.platform = "windows";
  profile.architecture = "x64";
  profile.configuration = "Development";
  profile.package.output = "out/package";
  profile.package.compression = "none";
  profile.content.rules.push_back({"**/*.chart.json",
                                   ProjectAssetTransform::Cook,
                                   ProjectAssetStorage::Pak, "audio", true});
  profile.content.rules.push_back({"**/*.performance.json",
                                   ProjectAssetTransform::Cook,
                                   ProjectAssetStorage::Pak, "audio", true});
  std::string error;
  CHECK(profile.save((root / "BuildProfiles/audio.bld").string(), &error));
  auto plan = ProjectBuildPlanner::create(profile, root.string());
  CHECK(plan.valid());
  auto first = ProjectBuildExecutor::execute(plan);
  CHECK_MSG(first.ok, first.error.c_str());
  CHECK(ayt::io::File::readAllText(
            (fs::path(first.outputDirectory) / "licenses/audio-content.txt")
                .string())
            .find("Alexander Holm") != std::string::npos);
  CHECK(first.cookedCount == 2);
  auto second = ProjectBuildExecutor::execute(plan);
  CHECK(second.ok);
  CHECK(second.cacheHitCount == 2);
  auto changed = pcm();
  std::vector<float> samples(48000, .2f);
  changed.setData(reinterpret_cast<const uint8_t *>(samples.data()),
                  samples.size() * 4);
  CHECK(changed.saveToBinary(music));
  write(assets / "audio/music.aud", music);
  const auto before = ayt::io::File::readAllText(first.manifestPath);
  const auto stale = ProjectBuildExecutor::execute(plan);
  CHECK(!stale.ok);
  CHECK(ayt::io::File::readAllText(first.manifestPath) == before);
  plan = ProjectBuildPlanner::create(profile, root.string());
  CHECK(plan.valid());
  const auto rebuilt = ProjectBuildExecutor::execute(plan);
  CHECK(rebuilt.ok);
  CHECK(rebuilt.cacheHitCount == 1);
  fs::rename(assets, root / "authoring-hidden");
  CHECK(initializeLoaders());
  auto &manager = ResourceManager::instance();
  manager.unloadAll();
  manager.setDatabase(nullptr);
  CHECK(manager.openDatabase(
      (fs::path(rebuilt.outputDirectory) / "Content/resources.db").string()));
  const auto loaded = manager.load<AudioContent>("audio/song.acb");
  CHECK(loaded != nullptr);
  if (loaded) {
    CHECK(loaded->events().size() == 2);
    CHECK(loaded->getChannels() == 1);
    CHECK(loaded->getSampleRate() == 48000);
  }
  const auto arranged = manager.load<AudioContent>("audio/piano.acb");
  CHECK(arranged != nullptr);
  manager.unloadAll();
  manager.setDatabase(nullptr);
}
TEST_SUITE_END
