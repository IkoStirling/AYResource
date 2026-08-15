#pragma once
// AYResource/Converter/AYResource/Converter/AYResource/Converter/TilemapConverter.h — CM-2 (2026-08-11): JSON -> .aytilemap v2 binary.
// Mirrors MaterialConverter structure (sourcePath/outputDir + setters +
// convert() -> ConversionResult). Output path MUST go through
// makeTilemapVirtualPath so cook tools, the loader, and the demo agree
// on one spelling.

#include "AYResource/IConverter.h"
#include <string>

namespace ayt::resource
{

// ===== TilemapConverter — 2D tilemap 转换器 =====
// Converts a .aytilemap.json author file into a .aytilemap v2 binary
// (magic 'AYTM', version 2 — the format TilemapLoader::loadFromBinary
// decodes; never invent a new magic/version).
//
// JSON schema:
//   {
//     "name": "ground",
//     "cols": 16, "rows": 12,
//     "tileWidth": 32, "tileHeight": 32,
//     "defaultTileId": 5,
//     "mode": "narrow16" | "wide32",          (default narrow16)
//     "tiles": [ flat row-major ids, optional; absent = all defaultTileId ],
//     "collisionFlags": [                      (optional)
//       { "tileId": 5, "flags": "solid|oneWay|slopeL|slopeR|hazard|ladder" }
//       // or "flags": "0x.." numeric bitmask
//     ]
//   }
class TilemapConverter : public IConverter {
public:
    TilemapConverter();
    explicit TilemapConverter(const std::string& sourcePath);
    virtual ~TilemapConverter() = default;

    // ===== IConverter =====
    void setSourcePath(const std::string& path) override;
    void setOutputDir(const std::string& dir) override;
    const std::string& getSourcePath() const { return sourcePath; }

    ConversionResult convert() override;
    const char* getSourceType() const override { return "Tilemap"; }

    bool isValid() const override { return !sourcePath.empty(); }

    // ===== 加载选项 =====
    void setLoadOption(IConverter::LoadOption option) override { loadOption = option; }
    IConverter::LoadOption getLoadOption() const { return loadOption; }

private:
    std::string sourcePath;
    std::string outputDir;
    LoadOption loadOption = IConverter::LoadOption::Full;
    ayt::math::FGuid lastGuid;
};

} // namespace ayt::resource
