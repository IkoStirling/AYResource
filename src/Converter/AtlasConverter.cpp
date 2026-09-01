#include "AYResource/Converter/AtlasConverter.h"

#include "AYResource/VirtualAssetPath.h"
#include "AYResource/assetsImpl/AtlasAsset.h"

#include <AYIO/File.h>
#include <AYSerializer.h>
#include <AYStorage/Guid.h>

namespace ayt::resource
{

namespace
{

std::string fileNameOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1u);
}

std::string atlasStem(std::string name)
{
    for (int i = 0; i < 2; ++i) {
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) break;
        name.resize(dot);
    }
    return name;
}

AtlasFilter parseFilter(const std::string& value)
{
    if (value == "nearest") return AtlasFilter::Nearest;
    if (value == "4tap" || value == "tap4") return AtlasFilter::Tap4;
    if (value == "9tap" || value == "tap9") return AtlasFilter::Tap9;
    return AtlasFilter::Linear;
}

AtlasWrap parseWrap(const std::string& value)
{
    if (value == "repeat") return AtlasWrap::Repeat;
    if (value == "mirror") return AtlasWrap::Mirror;
    return AtlasWrap::Clamp;
}

} // namespace

ConversionResult AtlasConverter::convert()
{
    ConversionResult result;
    if (!isValid()) return result;
    auto serializer = ayt::serializer::createSerializer(
        ayt::serializer::Format::Json);
    if (!serializer || !serializer->loadFromFile(_sourcePath)) return result;

    serializer->beginObject(nullptr);
    std::string name;
    std::string texturePath;
    std::string filter = "linear";
    std::string wrapU = "clamp";
    std::string wrapV = "clamp";
    UInt32 atlasWidth = 0u, atlasHeight = 0u;
    UInt32 tileWidth = 0u, tileHeight = 0u, gutter = 1u;
    serializer->field("name", name);
    serializer->field("texturePath", texturePath);
    serializer->field("atlasWidth", atlasWidth);
    serializer->field("atlasHeight", atlasHeight);
    serializer->field("tileWidth", tileWidth);
    serializer->field("tileHeight", tileHeight);
    serializer->field("gutter", gutter);
    serializer->field("filter", filter);
    serializer->field("wrapU", wrapU);
    serializer->field("wrapV", wrapV);
    serializer->endObject();

    AtlasAsset atlas;
    if (!atlas.create(texturePath, atlasWidth, atlasHeight,
                      tileWidth, tileHeight, gutter, parseFilter(filter),
                      parseWrap(wrapU), parseWrap(wrapV))) {
        return result;
    }
    std::vector<UInt8> bytes;
    if (!atlas.saveToBinary(bytes)) return result;

    const std::string stem = name.empty()
        ? atlasStem(fileNameOf(_sourcePath)) : name;
    const std::string virtualPath = makeAtlasVirtualPath(stem);
    if (!_outputDir.empty()) {
        const std::string output = _outputDir + "/" + virtualPath;
        if (!ayt::io::File::atomicWrite(output, bytes.data(), bytes.size())) {
            return result;
        }
    }

    ConversionResult::ConvertedResource converted;
    converted.guid = ayt::storage::Guid::computeFromData(bytes.data(), bytes.size());
    converted.path = virtualPath;
    converted.type = "Atlas";
    converted.size = static_cast<uint64_t>(bytes.size());
    result.resources.push_back(std::move(converted));
    return result;
}

} // namespace ayt::resource
