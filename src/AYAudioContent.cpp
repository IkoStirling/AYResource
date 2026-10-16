#include <AYAssetFormat/AssetFormat.h>
#include <AYResource/AudioContent.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
namespace ayt::resource {
namespace {
constexpr size_t MaxPcm = 256 * 1024 * 1024, MaxMetadata = 16 * 1024 * 1024;
uint32_t word(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}
bool valid(const Audio &a, AudioContent::Kind kind, double bpm, double offset,
           const std::vector<AudioContentEvent> &events) {
  if (kind != AudioContent::Kind::Performance &&
      kind != AudioContent::Kind::Chart)
    return false;
  if (!std::isfinite(bpm) || bpm < 20 || bpm > 1000 || !std::isfinite(offset) ||
      std::abs(offset) > 600 || events.size() > 100000 ||
      a.getSampleRate() < 8000 || a.getSampleRate() > 192000 ||
      a.getChannels() < 1 || a.getChannels() > 2 ||
      (a.getBitsPerSample() != 16 && a.getBitsPerSample() != 32) ||
      !a.getDataSize() || a.getDataSize() > MaxPcm)
    return false;
  const uint64_t stride =
                     uint64_t(a.getChannels()) * (a.getBitsPerSample() / 8),
                 frames = a.getDataSize() / stride;
  if (a.getDataSize() % stride || frames > uint64_t(a.getSampleRate()) * 600)
    return false;
  uint64_t last = 0;
  for (const auto &e : events) {
    if (e.beginFrame < last || e.beginFrame >= frames ||
        e.endFrame < e.beginFrame || e.endFrame > frames || e.note > 127 ||
        e.channel > 63 ||
        (kind == AudioContent::Kind::Chart && e.channel > 3) ||
        !std::isfinite(e.velocity) || e.velocity <= 0 || e.velocity > 1)
      return false;
    last = e.beginFrame;
  }
  if (a.getBitsPerSample() == 32)
    for (size_t i = 0; i < a.getDataSize(); i += 4) {
      float value;
      std::memcpy(&value, a.getData() + i, 4);
      if (!std::isfinite(value))
        return false;
    }
  return true;
}
} // namespace
bool AudioContent::assign(const Audio &audio, Kind kind, double bpm,
                          double offset, std::vector<AudioContentEvent> events,
                          std::string credits) {
  if (credits.size() > 65536 || !valid(audio, kind, bpm, offset, events))
    return false;
  static_cast<Audio &>(*this) = audio;
  _type = "AudioContent";
  _kind = kind;
  _bpm = bpm;
  _offset = offset;
  _events = std::move(events);
  _credits = std::move(credits);
  setLoaded(true);
  return true;
}
bool AudioContent::encode(std::vector<uint8_t> &out) const {
  if (!valid(*this, _kind, _bpm, _offset, _events))
    return false;
  nlohmann::json list = nlohmann::json::array();
  for (const auto &e : _events)
    list.push_back({e.beginFrame, e.endFrame, e.channel, e.note, e.velocity});
  const auto metadata = nlohmann::json{
      {"version", 1},
      {"kind", _kind == Kind::Performance ? "performance" : "chart"},
      {"bpm", _bpm},
      {"offset", _offset},
      {"events", list},
      {"credits", _credits}}.dump();
  if (metadata.size() > MaxMetadata)
    return false;
  std::vector<uint8_t> audio;
  if (!saveToBinary(audio) || audio.size() > MaxPcm + 1024)
    return false;
  std::vector<uint8_t> result;
  result.insert(result.end(), {'A', 'Y', 'A', 'C', 'B', '0', '0', '1'});
  auto add = [&](uint32_t value) {
    for (int i = 0; i < 4; ++i)
      result.push_back(uint8_t(value >> (i * 8)));
  };
  add(uint32_t(metadata.size()));
  add(uint32_t(audio.size()));
  result.insert(result.end(), metadata.begin(), metadata.end());
  result.insert(result.end(), audio.begin(), audio.end());
  out = std::move(result);
  return true;
}
bool AudioContent::decode(const void *data, size_t size) {
  try {
    if (!data || size < 16 || size > MaxPcm + MaxMetadata + 2048)
      return false;
    const auto *bytes = static_cast<const uint8_t *>(data);
    if (std::memcmp(bytes, "AYACB001", 8))
      return false;
    const uint32_t meta = word(bytes + 8), pcm = word(bytes + 12);
    if (!meta || meta > MaxMetadata || pcm > MaxPcm + 1024 ||
        uint64_t(meta) + pcm + 16 != size)
      return false;
    const auto j = nlohmann::json::parse(bytes + 16, bytes + 16 + meta);
    if (j.at("version") != 1)
      return false;
    const std::string type = j.at("kind");
    if (type != "performance" && type != "chart")
      return false;
    const auto &list = j.at("events");
    if (!list.is_array() || list.size() > 100000)
      return false;
    std::vector<AudioContentEvent> events;
    events.reserve(list.size());
    for (const auto &row : list) {
      if (!row.is_array() || row.size() != 5 || !row[0].is_number_unsigned() ||
          !row[1].is_number_unsigned() || !row[2].is_number_unsigned() ||
          !row[3].is_number_unsigned() || row[2].get<uint64_t>() > 63 ||
          row[3].get<uint64_t>() > 127)
        return false;
      events.push_back({row[0].get<uint64_t>(), row[1].get<uint64_t>(),
                        row[2].get<uint32_t>(), row[3].get<uint8_t>(),
                        row[4].get<float>()});
    }
    std::vector<uint8_t> aligned(bytes + 16 + meta, bytes + size);
    Audio audio;
    if (!audio.loadFromBinary(aligned.data(), pcm))
      return false;
    return assign(audio,
                  type == "performance" ? Kind::Performance : Kind::Chart,
                  j.at("bpm"), j.at("offset"), std::move(events),
                  j.value("credits", std::string{}));
  } catch (...) {
    return false;
  }
}
bool AudioContent::load(const std::string &path) {
  try {
    const auto file = std::filesystem::u8path(path).make_preferred();
    const auto length = std::filesystem::file_size(file);
    if (length > MaxPcm + MaxMetadata + 2048)
      return false;
    std::ifstream input(file, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
      return false;
    return decode(bytes.data(), bytes.size());
  } catch (...) {
    return false;
  }
}
bool AudioContent::unload() {
  _events.clear();
  _credits.clear();
  _bpm = 120;
  _offset = 0;
  return Audio::unload();
}
size_t AudioContent::sizeInBytes() const {
  return Audio::sizeInBytes() + _events.size() * sizeof(AudioContentEvent) +
         _credits.size();
}
bool AudioContentLoader::canLoad(const std::string &path) const {
  return asset_format::matchesPath(path, asset_format::Id::AudioContent);
}
std::shared_ptr<IResource> AudioContentLoader::load(const std::string &path) {
  auto result = std::make_shared<AudioContent>();
  return result->load(path) ? result : nullptr;
}
std::shared_ptr<IResource> AudioContentLoader::loadFromBinary(const void *bytes,
                                                              size_t size) {
  auto result = std::make_shared<AudioContent>();
  return result->decode(bytes, size) ? result : nullptr;
}
std::shared_ptr<IResource> AudioContentLoader::loadAsync(
    const std::string &path,
    std::function<void(std::shared_ptr<IResource>)> callback) {
  auto result = load(path);
  if (callback)
    callback(result);
  return result;
}
} // namespace ayt::resource
