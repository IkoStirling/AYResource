// AYTest_ResourcePipelineP5.cpp — P5: importAsset orchestration (no Assimp required)

#include "AYResource/ImportJob.h"
#include "AYTest.h"

#include <AYIO/File.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ayt::resource;

namespace {

const std::string kRoot = "ayresource_p5_tmp";

void cleanup()
{
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
}

bool writeText(const std::string& path, const std::string& text)
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string sampleDepJson()
{
    return R"({
  "resources": [
    {"path": "meshes/hero.aymesh", "type": "Mesh", "size": 12},
    {"path": "skeletons/hero.ayskel", "type": "Skeleton", "size": 8}
  ],
  "dependencies": [
    {"from": "meshes/hero.aymesh", "to": "materials/hero.aymat"}
  ]
}
)";
}

} // namespace

TEST_SUITE(ResourcePipelineP5Tests)

TEST_CASE(extension_helpers_match_editor_contract)
{
    CHECK(importExtensionOf("Foo.FBX") == "fbx");
    CHECK(importExtensionOf("a/b/c.Png") == "png");
    CHECK(importExtensionOf("noext") == "");
    CHECK(isImportSupportedExtension("a.fbx"));
    CHECK(isImportSupportedExtension("a.glb"));
    CHECK(isImportSupportedExtension("a.png"));
    CHECK(!isImportSupportedExtension("a.jpg"));
}

TEST_CASE(import_empty_and_missing_and_unsupported)
{
    ImportOptions opts;
    opts.outputDir = "cache";

    ImportResult empty = importAsset(opts);
    CHECK(!empty.ok);
    CHECK(empty.error.find("empty") != std::string::npos);

    opts.sourcePath = "D:/no/such/path/missing.fbx";
    ImportResult missing = importAsset(opts);
    CHECK(!missing.ok);
    CHECK(missing.error.find("does not exist") != std::string::npos);

    cleanup();
    const std::string bad = kRoot + "/stub.jpg";
    CHECK(writeText(bad, "fake"));
    opts.sourcePath = bad;
    opts.outputDir = kRoot + "/assets";
    ImportResult unsupported = importAsset(opts);
    CHECK(!unsupported.ok);
    CHECK(unsupported.error.find("unsupported") != std::string::npos);
    CHECK(unsupported.error.find("Supported:") != std::string::npos);
    cleanup();
}

TEST_CASE(import_reuses_aydep_cache_without_converter)
{
    cleanup();
    const std::string assets = kRoot + "/assets";
    const std::string src = kRoot + "/hero.fbx";
    CHECK(writeText(src, "not a real fbx — cache path must not convert"));
    CHECK(writeText(assets + "/meshes/hero.aymesh", "mesh-bytes"));
    CHECK(writeText(assets + "/skeletons/hero.ayskel", "skel-bytes"));
    CHECK(writeText(assets + "/hero.aydep.json", sampleDepJson()));

    ImportOptions opts;
    opts.sourcePath = src;
    opts.outputDir = assets;
    opts.force = false;
    opts.requireCharacterAssets = true;

    std::vector<ImportStage> stages;
    ImportResult r = importAsset(opts, [&](const ImportProgress& p) {
        stages.push_back(p.stage);
    });

    CHECK(r.ok);
    CHECK(r.usedCache);
    CHECK(r.conversion.resources.size() >= 2u);
    CHECK(!stages.empty());
    bool sawCacheHit = false;
    for (ImportStage s : stages) {
        if (s == ImportStage::CacheHit) {
            sawCacheHit = true;
        }
    }
    CHECK(sawCacheHit);
    cleanup();
}

TEST_CASE(import_cancel_before_convert)
{
    cleanup();
    const std::string assets = kRoot + "/assets";
    const std::string src = kRoot + "/hero.fbx";
    CHECK(writeText(src, "stub"));
    // No cache → would attempt convert; cancel after validate/check.

    ImportOptions opts;
    opts.sourcePath = src;
    opts.outputDir = assets;
    opts.force = true;

    ImportCancelToken token;
    token.requestCancel();

    ImportResult r = importAsset(opts, {}, &token);
    CHECK(!r.ok);
    CHECK(r.cancelled);
    CHECK(r.error.find("cancelled") != std::string::npos);
    cleanup();
}

TEST_CASE(import_batch_reports_per_item)
{
    cleanup();
    const std::string assets = kRoot + "/assets";
    const std::string a = kRoot + "/a.fbx";
    const std::string b = kRoot + "/b.fbx";
    CHECK(writeText(a, "stub-a"));
    CHECK(writeText(b, "stub-b"));
    CHECK(writeText(assets + "/meshes/a.aymesh", "m"));
    CHECK(writeText(assets + "/skeletons/a.ayskel", "s"));
    CHECK(writeText(assets + "/a.aydep.json",
                    R"({"resources":[{"path":"meshes/a.aymesh","type":"Mesh","size":1},{"path":"skeletons/a.ayskel","type":"Skeleton","size":1}],"dependencies":[]})"));
    // b has no cache → will fail at Assimp convert or empty result; with force
    // false and no sidecar, convert runs — may throw or return empty.
    // Use missing file for deterministic fail instead.
    const std::string missing = kRoot + "/missing.fbx";

    ImportBatchOptions batch;
    batch.sourcePaths = {a, missing};
    batch.outputDir = assets;
    batch.requireCharacterAssets = true;

    ImportBatchResult br = importAssetBatch(batch);
    CHECK(br.okCount == 1u);
    CHECK(br.failCount == 1u);
    CHECK(br.cacheHitCount == 1u);
    CHECK(br.results.size() == 2u);
    CHECK(br.results[0].ok);
    CHECK(br.results[0].usedCache);
    CHECK(!br.results[1].ok);
    cleanup();
}

TEST_SUITE_END
