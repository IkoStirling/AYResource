#pragma once

#include "aystorage/IPackageWriter.h"

#include <cstddef>
#include <string>

namespace ayt::resource
{

// ============================================================
// CookShip — offline ship builder (P4, library core)
//
// Walks a cooked assets root (.ay*), writes content.pak + resources.db
// for Runtime ResourceManager (DB/pak first, loose fallback).
// The CLI lives in AYTool/cook_tool; this header is the reusable API.
// ============================================================

struct CookShipOptions {
    std::string assetsRoot;                 ///< Input directory of cooked .ay* files
    std::string outputDir;                  ///< Output ship directory
    std::string pakFileName = "content.pak";
    std::string dbFileName = "resources.db";
    ayt::storage::CompressionAlgo compression = ayt::storage::CompressionAlgo::Zstd;
    bool recursive = true;
};

struct CookShipResult {
    bool ok = false;
    size_t fileCount = 0;
    size_t dependencyCount = 0;
    std::string pakPath;
    std::string dbPath;
    std::string error;
};

/// Build pak + StorageDatabase from an already-cooked asset tree.
CookShipResult cookShipPackage(const CookShipOptions& options);

} // namespace ayt::resource
