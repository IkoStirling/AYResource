#include "AYResource/ImportJob.h"
#include <AYResource/Converter/TextureConverter.h>
#include "AYResource/Loader/MeshLoader.h"
#include "AYResource/Loader/MaterialLoader.h"
#include <AYResource/Loader/TextureLoader.h>
#include <AYResource/assetsImpl/Texture.h>
#include <AYResource/assetsImpl/Material.h>
#include <nlohmann/json.hpp>
#include "AYResource/AssetPath.h"
#include "AYResource/LooseDependency.h"
#include "AYResource/assetsImpl/Mesh.h"
#include "AYResource/ResourceManager.h"
#include "AYResource/ResourceBootstrap.h"
#include "../src/ImportOutputTransaction.h"
#include "AYTest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <set>
#include <limits>
#include <cmath>
#include <cstdlib>
#if defined(_WIN32)
#include <Windows.h>
#endif

using namespace ayt::resource;

namespace {
namespace fs = std::filesystem;

struct ImportFixture {
    fs::path root = fs::temp_directory_path() / ("ay-import-lifecycle-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~ImportFixture() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path source() const { return root / "Source/Triangle.gltf"; }
    fs::path output() const { return root / "Imported"; }
    void write(const fs::path& path, const std::string& bytes) const {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    void triangle(float extent = 1.0f) const {
        const float positions[] = {0, 0, 0, extent, 0, 0, 0, extent, 0};
        write(root / "Source/Triangle.bin", std::string(
            reinterpret_cast<const char*>(positions), sizeof(positions)));
        write(source(), R"({"asset":{"version":"2.0"},
"buffers":[{"uri":"Triangle.bin","byteLength":36}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[4,4,0]}],
"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0},"mode":4}]}],
"nodes":[{"name":"Triangle","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
    }
    ImportOptions options() const {
        ImportOptions result;
        const auto sourceText = source().generic_u8string();
        const auto outputText = output().generic_u8string();
        result.sourcePath.assign(reinterpret_cast<const char*>(sourceText.data()), sourceText.size());
        result.outputDir.assign(reinterpret_cast<const char*>(outputText.data()), outputText.size());
        result.requireCharacterAssets = false;
        result.loadOption = IConverter::LoadOption::MeshOnly;
        return result;
    }
    std::string meshPath(const ImportResult& result) const {
        for (const auto& resource : result.conversion.resources)
            if (resource.type == "Mesh") return resource.path;
        return {};
    }
    std::string read(const fs::path& path) const {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
};
}

TEST_SUITE(ImportLifecycleTests)

TEST_CASE(gltf_import_preserves_authored_triangle_geometry)
{
    ImportFixture fixture;
    fixture.triangle();
    const auto result = importAsset(fixture.options());
    CHECK(result.ok);
    const auto path = fixture.meshPath(result);
    CHECK(!path.empty());
    if (path.empty()) return;
    MeshLoader loader;
    const auto loaded = std::dynamic_pointer_cast<Mesh>(loader.load(
        (fixture.output() / path).string()));
    CHECK(loaded != nullptr);
    if (!loaded) return;
    CHECK(loaded->getVertexCount() == 3u);
    CHECK(loaded->getIndexCount() == 3u);
}

TEST_CASE(invalid_model_import_reports_failure_without_publishing)
{
    ImportFixture fixture;
    fixture.write(fixture.source(), "invalid glTF bytes");
    const auto result = importAsset(fixture.options());
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
    CHECK(result.conversion.resources.empty());
}

TEST_CASE(forced_reimport_updates_geometry_and_preserves_resource_path)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    const auto first = importAsset(options);
    CHECK(first.ok);
    const auto path = fixture.meshPath(first);
    CHECK(!path.empty());
    const auto before = fixture.read(fixture.output() / path);
    fixture.triangle(2.0f);
    options.force = true;
    const auto second = importAsset(options);
    CHECK(second.ok);
    CHECK_FALSE(second.usedCache);
    CHECK(fixture.meshPath(second) == path);
    CHECK(fixture.read(fixture.output() / path) != before);
}

TEST_CASE(unchanged_import_reuses_fingerprinted_cache)
{
    ImportFixture fixture;
    fixture.triangle();
    const auto first = importAsset(fixture.options());
    const auto second = importAsset(fixture.options());
    CHECK(first.ok);
    CHECK(second.ok);
    CHECK(second.usedCache);
    CHECK(fixture.meshPath(first) == fixture.meshPath(second));
}

TEST_CASE(external_buffer_edit_with_preserved_timestamps_invalidates_cache)
{
    ImportFixture fixture;
    fixture.triangle();
    const auto first = importAsset(fixture.options());
    CHECK(first.ok);
    const auto path = fixture.meshPath(first);
    const auto before = fixture.read(fixture.output() / path);
    const auto sourceTime = fs::last_write_time(fixture.source());
    const auto buffer = fixture.root / "Source/Triangle.bin";
    const auto bufferTime = fs::last_write_time(buffer);
    fixture.triangle(2.0f);
    fs::last_write_time(fixture.source(), sourceTime);
    fs::last_write_time(buffer, bufferTime);
    const auto second = importAsset(fixture.options());
    CHECK(second.ok);
    CHECK_FALSE(second.usedCache);
    CHECK(fixture.meshPath(second) == path);
    CHECK(fixture.read(fixture.output() / path) != before);
}

TEST_CASE(source_edit_with_preserved_timestamp_invalidates_cache)
{
    ImportFixture fixture;
    fixture.triangle();
    CHECK(importAsset(fixture.options()).ok);
    const auto time = fs::last_write_time(fixture.source());
    auto text = fixture.read(fixture.source());
    text.insert(text.find("\"version\""), "\"generator\":\"changed\",");
    fixture.write(fixture.source(), text);
    fs::last_write_time(fixture.source(), time);
    const auto result = importAsset(fixture.options());
    CHECK(result.ok);
    CHECK_FALSE(result.usedCache);
}

TEST_CASE(changed_settings_with_same_tag_invalidate_cache)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.materialPolicy.tag = "same-preset";
    CHECK(importAsset(options).ok);
    options.materialPolicy.normalMapYSign = -1;
    const auto result = importAsset(options);
    CHECK(result.ok);
    CHECK_FALSE(result.usedCache);
}

TEST_CASE(corrupted_output_is_rebuilt_instead_of_reused)
{
    ImportFixture fixture;
    fixture.triangle();
    const auto first = importAsset(fixture.options());
    CHECK(first.ok);
    const auto path = fixture.output() / fixture.meshPath(first);
    const auto before = fixture.read(path);
    fixture.write(path, "corrupted mesh");
    const auto second = importAsset(fixture.options());
    CHECK(second.ok);
    CHECK_FALSE(second.usedCache);
    CHECK(fixture.read(path) == before);
}

TEST_CASE(same_basename_from_another_source_does_not_overwrite)
{
    ImportFixture fixture;
    fixture.triangle();
    const auto first = importAsset(fixture.options());
    CHECK(first.ok);
    const auto path = fixture.output() / fixture.meshPath(first);
    const auto before = fixture.read(path);
    const auto otherSource = fixture.root / "Other/Triangle.gltf";
    fixture.write(otherSource, fixture.read(fixture.source()));
    fixture.write(fixture.root / "Other/Triangle.bin", fixture.read(fixture.root / "Source/Triangle.bin"));
    auto options = fixture.options();
    options.sourcePath = otherSource.string();
    options.force = true;
    const auto result = importAsset(options);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("another source") != std::string::npos);
    CHECK(fixture.read(path) == before);
}

TEST_CASE(failed_reimport_preserves_previous_mesh_and_metadata)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    const auto first = importAsset(options);
    CHECK(first.ok);
    const auto path = fixture.output() / fixture.meshPath(first);
    const auto before = fixture.read(path);
    const auto metadata = fixture.read(first.depSidecarPath);
    fixture.write(fixture.source(), "broken source");
    options.force = true;
    const auto result = importAsset(options);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
    CHECK(fixture.read(path) == before);
    CHECK(fixture.read(first.depSidecarPath) == metadata);
}

TEST_CASE(cancelled_reimport_does_not_publish_staged_outputs)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    const auto first = importAsset(options);
    CHECK(first.ok);
    const auto path = fixture.output() / fixture.meshPath(first);
    const auto before = fixture.read(path);
    fixture.triangle(2.0f);
    options.force = true;
    ImportCancelToken token;
    const auto result = importAsset(options, [&](const ImportProgress& progress) {
        if (progress.stage == ImportStage::Convert) token.requestCancel();
    }, &token);
    CHECK_FALSE(result.ok);
    CHECK(result.cancelled);
    CHECK(fixture.read(path) == before);
}

TEST_CASE(timestamp_only_change_reuses_identical_content)
{
    ImportFixture fixture;
    fixture.triangle();
    CHECK(importAsset(fixture.options()).ok);
    fs::last_write_time(fixture.source(), fs::file_time_type::clock::now()
        + std::chrono::hours(1));
    const auto result = importAsset(fixture.options());
    CHECK(result.ok);
    CHECK(result.usedCache);
}

TEST_CASE(import_recipe_recovers_settings_and_tracks_missing_dependencies)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.materialPolicy.tag = "test \"preset\"";
    options.materialPolicy.blendNames = "Glass;Window";
    options.sourceCoordinates.tag = "source-preset";
    options.sourceCoordinates.uvOrigin = ImportUvOrigin::BottomLeft;
    const auto result = importAsset(options);
    CHECK(result.ok);
    ImportRecord record;
    std::string error;
    CHECK(readImportRecord(result.depSidecarPath, record, &error));
    CHECK(error.empty());
    CHECK(record.options.materialPolicy.tag == options.materialPolicy.tag);
    CHECK(record.options.materialPolicy.blendNames == options.materialPolicy.blendNames);
    CHECK(record.options.sourceCoordinates.uvOrigin == options.sourceCoordinates.uvOrigin);
    CHECK(record.options.loadOption == options.loadOption);
    CHECK(record.inputPaths.size() == 2u);
    CHECK(isImportRecordCurrent(record));
    fs::remove(fixture.root / "Source/Triangle.bin");
    CHECK_FALSE(isImportRecordCurrent(record));
}

TEST_CASE(invalid_import_recipe_preserves_callers_record)
{
    ImportFixture fixture;
    const auto path = fixture.root / "broken.dep";
    fixture.write(path, "{broken");
    ImportRecord record;
    record.options.sourcePath = "keep-source";
    record.sidecarPath = "keep-sidecar";
    std::string error;
    CHECK_FALSE(readImportRecord(path.string(), record, &error));
    CHECK_FALSE(error.empty());
    CHECK(record.options.sourcePath == "keep-source");
    CHECK(record.sidecarPath == "keep-sidecar");
}

TEST_CASE(conversion_metadata_roundtrips_escaped_names_and_rejects_malformed_json)
{
    ConversionResult conversion;
    conversion.materialPolicyTag = "quoted \"preset\"\\path\nnext";
    ConversionResult::ConvertedResource resource;
    resource.path = "meshes/part_\"quoted\".msh";
    resource.type = "Mesh";
    resource.size = 0xffffffffffffffffull;
    conversion.resources.push_back(resource);
    conversion.sourceFiles.push_back({"C:/Source/\"quoted\"/part.gltf", "digest", 42});
    const auto decoded = ConversionResult::fromJson(conversion.toJson());
    CHECK(decoded.resources.size() == 1u);
    if (!decoded.resources.empty()) {
        CHECK(decoded.resources[0].path == resource.path);
        CHECK(decoded.resources[0].size == resource.size);
    }
    CHECK(decoded.materialPolicyTag == conversion.materialPolicyTag);
    CHECK(decoded.sourceFiles == conversion.sourceFiles);
    CHECK(ConversionResult::fromJson("{\"resources\":[{\"path\":\"a\",\"type\":\"Mesh\"}]").resources.empty());
    CHECK(ConversionResult::fromJson("{\"resources\":[{\"path\":\"a\",\"type\":\"Mesh\",\"size\":-1}]}").resources.empty());
}

TEST_CASE(published_refresh_updates_resident_mesh_before_notifying_gpu_observer)
{
    ImportFixture fixture;
    fixture.triangle();
    const auto first = importAsset(fixture.options());
    CHECK(first.ok);
    auto& manager = ResourceManager::instance();
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(false);
    const auto path = (fixture.output() / fixture.meshPath(first)).string();
    const auto before = manager.load<Mesh>(path);
    CHECK(before != nullptr);
    if (!before) return;
    before->addTag({"keep-tag"});
    fixture.triangle(2.0f);
    auto options = fixture.options();
    options.force = true;
    CHECK(importAsset(options).ok);
    int notifications = 0;
    bool observerSawFreshMesh = false;
    manager.setOnHotReload([&](const std::string&) {
        ++notifications;
        observerSawFreshMesh = manager.load<Mesh>(path) != before;
    });
    std::string error;
    CHECK(manager.refreshPublishedResources({path, path}, &error));
    CHECK(error.empty());
    CHECK(notifications == 1);
    CHECK(observerSawFreshMesh);
    CHECK(manager.load<Mesh>(path)->hasTag({"keep-tag"}));
    CHECK(before->isLoaded()); // Existing borrowers retain the old owned object.
    manager.setOnHotReload({});
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(true);
}

TEST_CASE(published_refresh_handles_reported_directory_alias_and_physical_cache_keys_without_losing_old_borrowers)
{
    ImportFixture fixture;fixture.triangle();const auto first=importAsset(fixture.options());CHECK(first.ok);
    const auto alias=fixture.root/"ImportedAlias";
#if defined(_WIN32)
    const auto command=L"cmd /d /c mklink /J \""+alias.wstring()+L"\" \""+fixture.output().wstring()+L"\" >nul 2>&1";
    if(_wsystem(command.c_str())!=0){SKIP_TEST("Directory junction creation is unavailable");return;}
#else
    std::error_code linkError;fs::create_directory_symlink(fixture.output(),alias,linkError);
    if(linkError){SKIP_TEST("Directory symlink creation is unavailable");return;}
#endif
    auto& manager=ResourceManager::instance();manager.unloadAll();manager.setAutoWatchLoadedResources(false);
    const auto physical=(fixture.output()/fixture.meshPath(first)).string(),reported=(alias/fixture.meshPath(first)).string();
    auto before=manager.load<Mesh>(physical),aliasBefore=manager.load<Mesh>(reported);CHECK(before&&aliasBefore);
    if(!before||!aliasBefore){manager.unloadAll();manager.setAutoWatchLoadedResources(true);return;}
    before->addTag({"physical-tag"});aliasBefore->addTag({"alias-tag"});fixture.triangle(2);auto options=fixture.options();options.force=true;CHECK(importAsset(options).ok);
    int notifications=0;bool fresh=true;manager.setOnHotReload([&](const std::string&){++notifications;
        fresh=fresh&&manager.load<Mesh>(physical)!=before&&manager.load<Mesh>(reported)!=aliasBefore;});
    // Report only the physical spelling: the loaded junction spelling must
    // still be replaced/notified before observers see either cache entry.
    std::string error;CHECK(manager.refreshPublishedResources({physical,physical},&error));CHECK(error.empty());CHECK(notifications==2);CHECK(fresh);
    const auto current=manager.load<Mesh>(physical),aliasCurrent=manager.load<Mesh>(reported);CHECK(current->hasTag({"physical-tag"}));CHECK(aliasCurrent->hasTag({"alias-tag"}));
    CHECK(before->isLoaded()&&aliasBefore->isLoaded());fixture.write(fs::path(physical),"bad published mesh");notifications=0;
    CHECK_FALSE(manager.refreshPublishedResources({reported},&error));CHECK(notifications==0);CHECK(manager.load<Mesh>(physical)==current);CHECK(manager.load<Mesh>(reported)==aliasCurrent);
    manager.setOnHotReload({});manager.unloadAll();manager.setAutoWatchLoadedResources(true);
}

TEST_CASE(published_refresh_notifies_known_evicted_aliases_without_eagerly_loading_them)
{
    ImportFixture fixture;
    fixture.triangle();
    const auto first = importAsset(fixture.options());
    CHECK(first.ok);
    if (!first.ok) return;
    const auto alias = fixture.root / "EvictedAlias";
#if defined(_WIN32)
    const auto command = L"cmd /d /c mklink /J \"" + alias.wstring()
        + L"\" \"" + fixture.output().wstring() + L"\" >nul 2>&1";
    if (_wsystem(command.c_str()) != 0) {
        SKIP_TEST("Directory junction creation is unavailable"); return;
    }
#else
    std::error_code linkError;
    fs::create_directory_symlink(fixture.output(), alias, linkError);
    if (linkError) { SKIP_TEST("Directory symlink creation is unavailable"); return; }
#endif
    auto& manager = ResourceManager::instance();
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(false);
    ResourceCache::Config tiny;
    tiny.maxResourceCount = 1;
    tiny.weakGraceSeconds = 0;
    manager.setCacheConfig(tiny);
    const auto physical = fixture.output() / fixture.meshPath(first);
    const auto reported = alias / fixture.meshPath(first);
    std::weak_ptr<Mesh> expired;
    {
        const auto loaded = manager.load<Mesh>(reported.string());
        CHECK(loaded != nullptr);
        expired = loaded;
    }
    const auto otherPath = fixture.output() / "Other.msh";
    fixture.write(otherPath, fixture.read(physical));
    const auto other = manager.load<Mesh>(otherPath.string());
    CHECK(other != nullptr);
    CHECK(expired.expired());
    fixture.triangle(2);
    auto options = fixture.options(); options.force = true;
    const auto changed = importAsset(options);
    CHECK_MSG(changed.ok, changed.error.c_str());
    std::vector<std::string> notifications;
    manager.setOnHotReload([&](const auto& path) { notifications.push_back(path); });
    std::string error;
    CHECK_MSG(manager.refreshPublishedResources({physical.string()}, &error), error.c_str());
    const auto aliasKey = reported.lexically_normal().generic_u8string();
    CHECK(std::any_of(notifications.begin(), notifications.end(), [&](const auto& path) {
        return fs::u8path(path).lexically_normal().generic_u8string() == aliasKey;
    }));
    CHECK(notifications.size() == 2u);
    CHECK(expired.expired());
    CHECK_FALSE(manager.isLoaded(reported.string()));
    CHECK(manager.getResourceCount() == 1u);
    const auto demanded = manager.load<Mesh>(reported.string());
    CHECK(demanded != nullptr);
    if (demanded) CHECK_FLOAT_EQ(demanded->getBounds().getMin().x, -2, .0001f);
    manager.setOnHotReload({});
    manager.unloadAll();
    manager.setCacheConfig(ResourceCache::Config{});
    manager.setAutoWatchLoadedResources(true);
}

TEST_CASE(failed_published_refresh_preserves_all_resident_entries_and_sends_no_notification)
{
    ImportFixture fixture;
    fixture.triangle();
    const auto first = importAsset(fixture.options());
    CHECK(first.ok);
    auto& manager = ResourceManager::instance();
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(false);
    const auto path = fixture.output() / fixture.meshPath(first);
    const auto other = fixture.output() / "meshes/Other.msh";
    fixture.write(other, fixture.read(path));
    const auto before = manager.load<Mesh>(path.string());
    const auto beforeOther = manager.load<Mesh>(other.string());
    CHECK(before != nullptr);
    CHECK(beforeOther != nullptr);
    fixture.triangle(2.0f);
    auto options = fixture.options();
    options.force = true;
    CHECK(importAsset(options).ok);
    fixture.write(other, "corrupt published payload");
    int notifications = 0;
    manager.setOnHotReload([&](const std::string&) { ++notifications; });
    std::string error;
    CHECK_FALSE(manager.refreshPublishedResources({path.string(), other.string()}, &error));
    CHECK_FALSE(error.empty());
    CHECK(notifications == 0);
    CHECK(manager.load<Mesh>(path.string()) == before);
    CHECK(manager.load<Mesh>(other.string()) == beforeOther);
    manager.setOnHotReload({});
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(true);
}

TEST_CASE(dependency_changed_after_conversion_is_refused_before_publication)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    const auto first = importAsset(options);
    CHECK(first.ok);
    const auto path = fixture.output() / fixture.meshPath(first);
    const auto before = fixture.read(path);
    const auto metadata = fixture.read(first.depSidecarPath);
    options.force = true;
    const auto result = importAsset(options, [&](const ImportProgress& progress) {
        if (progress.stage == ImportStage::Convert && progress.fraction > .8f)
            fixture.triangle(2.0f);
    });
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("changed during import") != std::string::npos);
    CHECK(fixture.read(path) == before);
    CHECK(fixture.read(first.depSidecarPath) == metadata);
}

TEST_CASE(gltf_import_uses_utf8_source_and_output_paths)
{
    ImportFixture fixture;
    fixture.root += fs::u8path("-导入 测试");
    fixture.triangle();
    const auto result = importAsset(fixture.options());
    if (!result.ok) std::fprintf(stderr, "[UTF8 import diagnostic] %s\n", result.error.c_str());
    CHECK(result.ok);
    CHECK_FALSE(fixture.meshPath(result).empty());
    if (!result.ok) return;
    CHECK(fs::is_regular_file(fixture.output() / fs::u8path(fixture.meshPath(result))));
    const auto cached = importAsset(fixture.options());
    CHECK(cached.ok);
    CHECK(cached.usedCache);
}

TEST_CASE(glb_import_reads_its_real_embedded_geometry)
{
    ImportFixture fixture;
    fixture.triangle();
    auto json = fixture.read(fixture.source());
    const std::string uri = "\"uri\":\"Triangle.bin\",";
    json.erase(json.find(uri), uri.size());
    while (json.size() % 4) json.push_back(' ');
    const auto binary = fixture.read(fixture.root / "Source/Triangle.bin");
    const auto appendWord = [](std::string& target, std::uint32_t word) {
        for (int byte = 0; byte < 4; ++byte) target.push_back(char((word >> (byte * 8)) & 255));
    };
    std::string glb;
    appendWord(glb, 0x46546c67); appendWord(glb, 2);
    appendWord(glb, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + binary.size()));
    appendWord(glb, static_cast<std::uint32_t>(json.size())); appendWord(glb, 0x4e4f534a);
    glb += json;
    appendWord(glb, static_cast<std::uint32_t>(binary.size())); appendWord(glb, 0x004e4942);
    glb += binary;
    auto source = fixture.source(); source.replace_extension(".glb");
    fixture.write(source, glb);
    auto options = fixture.options();
    options.sourcePath = source.string();
    const auto result = importAsset(options);
    CHECK(result.ok);
    const auto path = fixture.meshPath(result);
    CHECK_FALSE(path.empty());
    if (path.empty()) return;
    MeshLoader loader;
    const auto mesh = std::dynamic_pointer_cast<Mesh>(loader.load((fixture.output() / path).string()));
    CHECK(mesh != nullptr);
    if (mesh) { CHECK(mesh->getVertexCount() == 3u); CHECK(mesh->getIndexCount() == 3u); }
    CHECK(importAsset(options).usedCache);
}

TEST_CASE(authoring_recipe_codec_preserves_portable_settings_without_session_flags)
{
    ImportOptions options;
    options.sourcePath = "Assets/Models/设备.gltf";
    options.outputDir = ".ayeditor_cache/assets";
    options.sourceAssetRoot = "Assets";
    options.sourceId = "stable-source";
    options.outputNamespace = "source-folder";
    options.requireCharacterAssets = false;
    options.force = true;
    options.allowLegacyCache = true;
    options.materialPolicy.tag = "author \"settings\"";
    options.materialPolicy.blendNames = "Window;Glass";
    options.sourceCoordinates.mode = SourceCoordinateMode::Manual;
    options.sourceCoordinates.up = ImportAxis::PositiveZ;
    options.sourceCoordinates.forward = ImportAxis::PositiveY;
    options.sourceCoordinates.metersPerUnit = .01f;
    ImportOptions decoded;
    std::string error;
    CHECK(parseImportRecipe(serializeImportRecipe(options), decoded, &error));
    CHECK(error.empty());
    CHECK(decoded.sourcePath == options.sourcePath);
    CHECK(decoded.outputDir == options.outputDir);
    CHECK(decoded.sourceAssetRoot == options.sourceAssetRoot);
    CHECK(decoded.sourceId == options.sourceId);
    CHECK(decoded.outputNamespace == options.outputNamespace);
    CHECK(decoded.materialPolicy.tag == options.materialPolicy.tag);
    CHECK(decoded.materialPolicy.blendNames == options.materialPolicy.blendNames);
    CHECK(decoded.sourceCoordinates.mode == options.sourceCoordinates.mode);
    CHECK(decoded.sourceCoordinates.up == options.sourceCoordinates.up);
    CHECK_FLOAT_EQ(decoded.sourceCoordinates.metersPerUnit, .01f, 1e-6f);
    CHECK_FALSE(decoded.force);
    CHECK_FALSE(decoded.allowLegacyCache);
    decoded.sourcePath = "keep-existing";
    CHECK_FALSE(parseImportRecipe("{broken", decoded, &error));
    CHECK(decoded.sourcePath == "keep-existing");
    CHECK_FALSE(error.empty());
}

TEST_CASE(source_namespaces_isolate_identically_named_models_and_material_references)
{
    ImportFixture fixture;
    fixture.triangle();
    auto firstOptions = fixture.options();
    firstOptions.sourceId = "owner-a";
    firstOptions.outputNamespace = "source-a";
    firstOptions.loadOption = IConverter::LoadOption::Full;
    const auto first = importAsset(firstOptions);
    CHECK(first.ok);
    const auto firstPath = fixture.meshPath(first);
    const auto before = fixture.read(fixture.output() / firstPath);
    CHECK(firstPath.rfind("meshes/source-a/", 0) == 0u);
    const auto secondSource = fixture.root / "Other/Triangle.gltf";
    fixture.write(secondSource, fixture.read(fixture.source()));
    fixture.write(fixture.root / "Other/Triangle.bin", fixture.read(fixture.root / "Source/Triangle.bin"));
    auto secondOptions = firstOptions;
    secondOptions.sourcePath = secondSource.string();
    secondOptions.sourceId = "owner-b";
    secondOptions.outputNamespace = "source-b";
    const auto second = importAsset(secondOptions);
    CHECK(second.ok);
    CHECK(fixture.meshPath(second).rfind("meshes/source-b/", 0) == 0u);
    CHECK(fixture.read(fixture.output() / firstPath) == before);
    CHECK(first.depSidecarPath != second.depSidecarPath);
    CHECK(importAsset(firstOptions).usedCache);
    CHECK(importAsset(secondOptions).usedCache);
    MeshLoader loader;
    const auto mesh = std::dynamic_pointer_cast<Mesh>(loader.load((fixture.output() / firstPath).string()));
    CHECK(mesh && mesh->getMaterialSlotCount() > 0);
    if (mesh && mesh->getMaterialSlotCount()) {
        const std::string material = mesh->getMaterialSlot(0);
        CHECK(material.rfind("materials/source-a/", 0) == 0u);
        CHECK(fs::is_regular_file(fixture.output() / material));
    }
    const auto sidecar = looseDependencySidecarPath((fixture.output() / firstPath).string());
    CHECK(fs::is_regular_file(sidecar));
}

TEST_CASE(bound_source_identity_allows_relocated_source_without_changing_asset_paths)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.sourceId = "stable-source";
    options.outputNamespace = "source-a";
    const auto first = importAsset(options);
    CHECK(first.ok);
    const auto before = fixture.read(fixture.output() / fixture.meshPath(first));
    fs::rename(fixture.root / "Source", fixture.root / "Relocated");
    options.sourcePath = (fixture.root / "Relocated/Triangle.gltf").string();
    const auto second = importAsset(options);
    CHECK(second.ok);
    CHECK_FALSE(second.usedCache);
    CHECK(fixture.meshPath(first) == fixture.meshPath(second));
    CHECK(fixture.read(fixture.output() / fixture.meshPath(second)) == before);
    ImportRecord record;
    CHECK(readImportRecord(second.depSidecarPath, record));
    CHECK(record.options.sourceId == options.sourceId);
    CHECK(record.options.outputNamespace == options.outputNamespace);
    CHECK(isImportRecordCurrent(record));
}

TEST_CASE(invalid_namespace_does_not_write_outside_the_import_root)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.outputNamespace = "../escape";
    const auto result = importAsset(options);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("namespace") != std::string::npos);
    CHECK_FALSE(fs::exists(fixture.root / "escape"));
}

#if defined(_WIN32)
TEST_CASE(publication_retries_a_transient_metadata_reader_without_replacing_unrelated_files)
{
    ImportFixture fixture;
    fixture.write(fixture.output() / "A.bin", "previous-a");
    fixture.write(fixture.output() / "source.dep", "previous-metadata");
    fixture.write(fixture.output() / "Unrelated.bin", "keep");
    detail::ImportOutputTransaction transaction(fixture.output().string());
    fixture.write(fs::u8path(transaction.stagingRoot()) / "A.bin", "new-a");
    fixture.write(fs::u8path(transaction.stagingRoot()) / "source.dep", "new-metadata");
    HANDLE reader = ::CreateFileW((fixture.output() / "source.dep").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(reader != INVALID_HANDLE_VALUE);
    if (reader == INVALID_HANDLE_VALUE) return;
    std::size_t retries = 0;
    bool metadataWasLast = false;
    bool published = false;
    std::string failure;
    try {
        transaction.publish("source.dep", [&](std::size_t attempt, const std::error_code& error) {
            ++retries;
            CHECK(attempt >= 2u);
            CHECK(ayt::io::detail::isWindowsPublishContention(error));
            metadataWasLast = fixture.read(fixture.output() / "A.bin") == "new-a"
                && fixture.read(fixture.output() / "source.dep") == "previous-metadata";
            // Release only after the real filesystem operation encountered the
            // held reader, so this regression does not depend on a timed sleep.
            if (reader != INVALID_HANDLE_VALUE) {
                ::CloseHandle(reader);
                reader = INVALID_HANDLE_VALUE;
            }
        });
        published = true;
    } catch (const std::exception& error) {
        failure = error.what();
    }
    if (reader != INVALID_HANDLE_VALUE) ::CloseHandle(reader);
    CHECK_MSG(published, failure.c_str());
    CHECK(retries > 0u);
    CHECK(metadataWasLast);
    CHECK(fixture.read(fixture.output() / "A.bin") == "new-a");
    CHECK(fixture.read(fixture.output() / "source.dep") == "new-metadata");
    CHECK(fixture.read(fixture.output() / "Unrelated.bin") == "keep");
}

TEST_CASE(publication_failure_after_a_replacement_rolls_back_previous_files)
{
    ImportFixture fixture;
    fixture.write(fixture.output() / "A.bin", "previous-a");
    fixture.write(fixture.output() / "B.bin", "previous-b");
    detail::ImportOutputTransaction transaction(fixture.output().string());
    fixture.write(fs::u8path(transaction.stagingRoot()) / "A.bin", "new-a");
    fixture.write(fs::u8path(transaction.stagingRoot()) / "B.bin", "new-b");
    const auto lock = ::CreateFileW((fixture.output() / "B.bin").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    if (lock == INVALID_HANDLE_VALUE) return;
    bool failed = false;
    try { transaction.publish(); } catch (const std::exception&) { failed = true; }
    ::CloseHandle(lock);
    CHECK(failed);
    CHECK(fixture.read(fixture.output() / "A.bin") == "previous-a");
    CHECK(fixture.read(fixture.output() / "B.bin") == "previous-b");
}
#endif


TEST_CASE(gltf_packed_material_channels_match_source_and_texture_edits_invalidate_cache)
{
    ImportFixture fixture;
    fixture.triangle();
    const std::string hex = "89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    std::string png;
    for (std::size_t i=0;i<hex.size();i+=2) png.push_back(char(std::stoi(hex.substr(i,2),nullptr,16)));
    fixture.write(fixture.root / "Source/Surface.png", png);
    auto source = nlohmann::json::parse(fixture.read(fixture.source()));
    source["images"] = {{{"uri","Surface.png"}}};
    source["textures"] = {{{"source",0}}};
    source["materials"] = {{{"pbrMetallicRoughness",
        {{"baseColorTexture",{{"index",0}}},{"metallicRoughnessTexture",{{"index",0}}}}},
        {"occlusionTexture",{{"index",0}}}}};
    source["meshes"][0]["primitives"][0]["material"] = 0;
    fixture.write(fixture.source(), source.dump());
    auto options = fixture.options();
    options.loadOption = IConverter::LoadOption::Full;
    const auto first = importAsset(options);
    CHECK_MSG(first.ok, first.error.c_str());
    if (!first.ok) return;
    std::string materialPath;
    for (const auto& output : first.conversion.resources)
        if (output.type=="Material") {materialPath=output.path;break;}
    MaterialLoader materialLoader;
    const auto material = std::dynamic_pointer_cast<Material>(materialLoader.load((fixture.output()/materialPath).string()));
    CHECK(material != nullptr);
    if (!material) return;
    TextureLoader textureLoader;
    for (const auto& [slot, expected] : std::vector<std::pair<const char*,unsigned>>{
        {"baseColorTexture",17}, {"aoTexture",17}, {"roughnessTexture",85}, {"metallicTexture",204}}) {
        const auto* path = material->getTexture(slot);
        CHECK(path && *path);
        if (!path || !*path) continue;
        const auto texture = std::dynamic_pointer_cast<Texture>(textureLoader.load((fixture.output()/path).string()));
        CHECK(texture != nullptr);
        if (texture) {
            CHECK(texture->getWidth()==2 && texture->getHeight()==2);
            CHECK(texture->getImageDataSize()>=16);
            CHECK_MSG(texture->getImageDataBytes()[0]==expected, slot);
        }
    }
    CHECK(importAsset(options).usedCache);
    const auto stamp = fs::last_write_time(fixture.root / "Source/Surface.png");
    std::string changed;
    const std::string changedHex="89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63540a3df39f81818181094480300021ae0246c539b8700000000049454e44ae426082";
    for(std::size_t i=0;i<changedHex.size();i+=2) changed.push_back(char(std::stoi(changedHex.substr(i,2),nullptr,16)));
    CHECK(changed.size()==png.size());
    fixture.write(fixture.root / "Source/Surface.png",changed);
    fs::last_write_time(fixture.root / "Source/Surface.png",stamp);
    const auto updated=importAsset(options);
    CHECK_MSG(updated.ok,updated.error.c_str());
    CHECK_FALSE(updated.usedCache);
    const auto meshBytes=fixture.read(fixture.output()/fixture.meshPath(updated));
    fs::remove(fixture.root/"Source/Surface.png");
    const auto missing=importAsset(options);
    CHECK_FALSE(missing.ok);
    CHECK(fixture.read(fixture.output()/fixture.meshPath(updated))==meshBytes);
}

TEST_CASE(gltf_embedded_png_material_is_published_with_resolvable_texture_binding)
{
    ImportFixture fixture;
    fixture.triangle();
    auto source = nlohmann::json::parse(fixture.read(fixture.source()));
    source["images"] = {{{"uri","data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFUlEQVR4nGMUDD3zn4GBgYEJRIAwACCNAjWTO8BCAAAAAElFTkSuQmCC"}}};
    source["textures"] = {{{"source",0}}};
    source["materials"] = {{{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",0}}}}}}};
    source["meshes"][0]["primitives"][0]["material"]=0;
    fixture.write(fixture.source(),source.dump());
    auto options=fixture.options();
    options.loadOption=IConverter::LoadOption::Full;
    options.outputNamespace="embedded-source";
    const auto imported=importAsset(options);
    CHECK_MSG(imported.ok,imported.error.c_str());
    if(!imported.ok)return;
    std::string materialPath;
    for(const auto& output:imported.conversion.resources)
        if(output.type=="Material"){materialPath=output.path;break;}
    MaterialLoader materialLoader;
    const auto material=std::dynamic_pointer_cast<Material>(materialLoader.load((fixture.output()/materialPath).string()));
    CHECK(material!=nullptr);
    if(!material)return;
    const auto* path=material->getTexture("baseColorTexture");
    CHECK(path && *path);
    if(!path || !*path)return;
    CHECK(std::string(path).find("textures/embedded-source/")==0);
    TextureLoader textureLoader;
    const auto texture=std::dynamic_pointer_cast<Texture>(textureLoader.load((fixture.output()/path).string()));
    CHECK(texture!=nullptr);
    if(texture){
        CHECK(texture->getWidth()==2 && texture->getHeight()==2);
        CHECK(texture->getImageDataBytes()[0]==17);
        CHECK(texture->getImageDataBytes()[1]==85);
        CHECK(texture->getImageDataBytes()[2]==204);
    }
    CHECK(importAsset(options).usedCache);
}


TEST_CASE(import_recipe_rejects_fractional_wrapped_and_degenerate_coordinate_enums)
{
    ImportFixture fixture;
    auto options=fixture.options();
    const auto text=serializeImportRecipe(options);
    for(const auto& invalid:nlohmann::json::array({1.5,4294967296ULL,-1})){
        auto json=nlohmann::json::parse(text);
        json["settings"]["loadOption"]=invalid;
        auto candidate=options;
        CHECK_FALSE(parseImportRecipe(json.dump(),candidate));
        CHECK(candidate.sourcePath==options.sourcePath);
    }
    auto json=nlohmann::json::parse(text);
    json["settings"]["coordinates"]["mode"]=1;
    json["settings"]["coordinates"]["forward"]=json["settings"]["coordinates"]["up"];
    CHECK_FALSE(parseImportRecipe(json.dump(),options));
}


TEST_CASE(decoded_image_mips_handle_single_pixel_axes_and_refuse_unsupported_encoders)
{
    ImportFixture fixture;
    TextureConverter converter;
    converter.setOutputDir(fixture.output().string());
    converter.setOutputFormat(TextureFormat::RGBA8);
    TextureData image;
    image.width=1;image.height=3;
    image.imageData={255,0,0,255, 0,255,0,255, 0,0,255,255};
    const auto result=converter.convertPixels(image,"textures/odd.txr");
    CHECK(result.resources.size()==1u);
    TextureLoader loader;
    const auto texture=std::dynamic_pointer_cast<Texture>(loader.load(
        (fixture.output()/"textures/odd.txr").string()));
    CHECK(texture!=nullptr);
    if(texture){
        CHECK(texture->getMipmapCount()==2u);
        CHECK(texture->getMipmapSize(0)==12u);
        CHECK(texture->getMipmapSize(1)==4u);
        const auto* mip=texture->getMipmapData(1);
        CHECK(mip && mip[0]==127 && mip[1]==127 && mip[2]==0 && mip[3]==255);
    }
    for(auto unsupported:{TextureFormat::BC1,TextureFormat::BC5,TextureFormat::RGB8}){
        converter.setOutputFormat(unsupported);
        CHECK(converter.convertPixels(image,"textures/refused.txr").resources.empty());
        CHECK_FALSE(fs::exists(fixture.output()/"textures/refused.txr"));
    }
    converter.setOutputFormat(TextureFormat::RGBA8);
    image.imageData.pop_back();
    CHECK(converter.convertPixels(image,"textures/invalid.txr").resources.empty());
}


TEST_CASE(gltf_parent_directory_texture_reference_uses_a_safe_derived_name)
{
    ImportFixture fixture;
    fixture.triangle();
    const std::string hex="89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    std::string bytes;
    for(std::size_t i=0;i<hex.size();i+=2)bytes.push_back(char(std::stoi(hex.substr(i,2),nullptr,16)));
    fixture.write(fixture.root/"Textures/Surface.png",bytes);
    auto source=nlohmann::json::parse(fixture.read(fixture.source()));
    source["images"]={{{"uri","../Textures/Surface.png"}}};
    source["textures"]={{{"source",0}}};
    source["materials"]={{{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",0}}}}}}};
    source["meshes"][0]["primitives"][0]["material"]=0;
    fixture.write(fixture.source(),source.dump());
    auto options=fixture.options();
    options.outputNamespace="parent-textures";
    const auto imported=importAsset(options);
    CHECK_MSG(imported.ok,imported.error.c_str());
    if(!imported.ok)return;
    bool found=false;
    for(const auto& output:imported.conversion.resources)if(output.type=="Texture"){
        found=true;
        CHECK(output.path.rfind("textures/parent-textures/",0)==0);
        CHECK(ayt::io::path::isPortableRelative(output.path));
        CHECK(fixture.read(fixture.output()/output.path)==bytes);
    }
    CHECK(found);
    CHECK(importAsset(options).usedCache);
}

TEST_CASE(gltf_distinct_images_with_colliding_flat_names_keep_both_bindings)
{
    ImportFixture fixture;
    fixture.triangle();
    const std::string blueHex="89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    const std::string redHex="89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c637c6620f69f81818181094480300021b8022f99fc55610000000049454e44ae426082";
    const auto bytes=[](const std::string& hex){
        std::string result;
        for(std::size_t i=0;i<hex.size();i+=2)result.push_back(char(std::stoi(hex.substr(i,2),nullptr,16)));
        return result;
    };
    fixture.write(fixture.root/"Source/one/Surface.png",bytes(blueHex));
    fixture.write(fixture.root/"Source/one_Surface.png",bytes(redHex));
    auto source=nlohmann::json::parse(fixture.read(fixture.source()));
    source["images"]={{{"uri","one/Surface.png"}},{{"uri","one_Surface.png"}}};
    source["textures"]={{{"source",0}},{{"source",1}}};
    source["materials"]={
        {{"name","Blue"},{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",0}}}}}},
        {{"name","Red"},{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",1}}}}}}
    };
    source["meshes"][0]["primitives"]=nlohmann::json::array({
        {{"attributes",{{"POSITION",0}}},{"material",0}},
        {{"attributes",{{"POSITION",0}}},{"material",1}}
    });
    fixture.write(fixture.source(),source.dump());
    auto options=fixture.options();
    options.outputNamespace="colliding-textures";
    const auto imported=importAsset(options);
    CHECK_MSG(imported.ok,imported.error.c_str());
    if(!imported.ok)return;
    MaterialLoader materialLoader;
    TextureLoader textureLoader;
    std::set<std::string> textures;
    std::set<unsigned> values;
    for(const auto& output:imported.conversion.resources)if(output.type=="Material"){
        const auto material=std::dynamic_pointer_cast<Material>(materialLoader.load((fixture.output()/output.path).string()));
        CHECK(material!=nullptr);
        if(!material)continue;
        const auto* path=material->getTexture("baseColorTexture");
        if(!path || !*path)continue;
        textures.insert(path);
        const auto texture=std::dynamic_pointer_cast<Texture>(textureLoader.load((fixture.output()/path).string()));
        CHECK(texture!=nullptr);
        if(texture)values.insert(texture->getImageDataBytes()[0]);
    }
    CHECK(textures.size()==2u);
    CHECK(values==std::set<unsigned>({17,230}));
    options.force=true;
    const auto repeated=importAsset(options);
    CHECK_MSG(repeated.ok,repeated.error.c_str());
    std::set<std::string> repeatedTextures;
    for(const auto& output:repeated.conversion.resources)if(output.type=="Texture")repeatedTextures.insert(output.path);
    CHECK(repeatedTextures==textures);
}


TEST_CASE(import_api_rejects_invalid_settings_before_creating_derived_output)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options=fixture.options();
    for(float invalid:{-1.0f,(std::numeric_limits<float>::infinity)(),
            (std::numeric_limits<float>::quiet_NaN)()}){
        options.sourceCoordinates.metersPerUnit=invalid;
        const auto result=importAsset(options);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("invalid import settings")!=std::string::npos);
        CHECK_FALSE(fs::exists(fixture.output()));
    }
    options=fixture.options();
    options.loadOption=static_cast<IConverter::LoadOption>(-1);
    CHECK_FALSE(importAsset(options).ok);
    options=fixture.options();
    options.sourceCoordinates.mode=SourceCoordinateMode::Manual;
    options.sourceCoordinates.forward=options.sourceCoordinates.up;
    CHECK_FALSE(importAsset(options).ok);
    CHECK_FALSE(fs::exists(fixture.output()));
}


TEST_CASE(glb_buffer_view_png_binding_and_cooked_reimport_use_real_embedded_pixels)
{
    ImportFixture fixture;
    fixture.triangle();
    auto source=nlohmann::json::parse(fixture.read(fixture.source()));
    auto binary=fixture.read(fixture.root/"Source/Triangle.bin");
    const std::string hex="89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    const auto imageOffset=binary.size();
    for(std::size_t i=0;i<hex.size();i+=2)binary.push_back(char(std::stoi(hex.substr(i,2),nullptr,16)));
    source["buffers"][0].erase("uri");
    source["buffers"][0]["byteLength"]=binary.size();
    source["bufferViews"].push_back({{"buffer",0},{"byteOffset",imageOffset},{"byteLength",hex.size()/2}});
    source["images"]={{{"bufferView",1},{"mimeType","image/png"}}};
    source["textures"]={{{"source",0}}};
    source["materials"]={{{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",0}}}}}}};
    source["meshes"][0]["primitives"][0]["material"]=0;
    auto json=source.dump();
    while(json.size()%4)json.push_back(' ');
    while(binary.size()%4)binary.push_back('\0');
    const auto word=[](std::string& target,std::uint32_t value){
        for(int byte=0;byte<4;++byte)target.push_back(char((value>>(byte*8))&255));
    };
    std::string glb;
    word(glb,0x46546c67);word(glb,2);word(glb,std::uint32_t(12+8+json.size()+8+binary.size()));
    word(glb,std::uint32_t(json.size()));word(glb,0x4e4f534a);glb+=json;
    word(glb,std::uint32_t(binary.size()));word(glb,0x004e4942);glb+=binary;
    const auto path=fixture.root/"Source/Embedded.glb";
    fixture.write(path,glb);
    auto options=fixture.options();
    options.sourcePath=path.string(); options.outputNamespace="glb-images";
    const auto imported=importAsset(options);
    CHECK_MSG(imported.ok,imported.error.c_str());
    if(!imported.ok)return;
    MeshLoader meshLoader;
    const auto mesh=std::dynamic_pointer_cast<Mesh>(meshLoader.load(
        (fixture.output()/fixture.meshPath(imported)).string()));
    CHECK(mesh && mesh->getMaterialSlotCount()>0);
    if(!mesh || !mesh->getMaterialSlotCount())return;
    MaterialLoader materialLoader;
    const auto material=std::dynamic_pointer_cast<Material>(materialLoader.load(
        (fixture.output()/mesh->getMaterialSlot(0)).string()));
    CHECK(material!=nullptr);
    if(!material)return;
    const auto* texturePath=material->getTexture("baseColorTexture");
    CHECK(texturePath && *texturePath);
    if(!texturePath || !*texturePath)return;
    TextureLoader textureLoader;
    const auto texture=std::dynamic_pointer_cast<Texture>(textureLoader.load((fixture.output()/texturePath).string()));
    CHECK(texture!=nullptr);
    if(texture){
        CHECK(texture->getFormat()==TextureFormat::RGBA8);
        CHECK(texture->getImageDataBytes()[0]==17);
        CHECK(texture->getImageDataBytes()[1]==85);
        CHECK(texture->getImageDataBytes()[2]==204);
    }
    ImportRecord record;
    CHECK(readImportRecord(imported.depSidecarPath,record));
    CHECK(record.inputPaths.size()==1u);
    CHECK(importAsset(options).usedCache);
    options.cookTextures=true;
    const auto cooked=importAsset(options);
    CHECK_MSG(cooked.ok,cooked.error.c_str());
    CHECK_FALSE(cooked.usedCache);
    const auto cookedTexture=std::dynamic_pointer_cast<Texture>(textureLoader.load((fixture.output()/texturePath).string()));
    CHECK(cookedTexture!=nullptr);
    if(cookedTexture){
        CHECK(cookedTexture->getFormat()==TextureFormat::BC7);
        CHECK(cookedTexture->getMipmapCount()==2u);
    }
}


TEST_CASE(batch_texture_writer_preserves_odd_mips_and_rewrites_changed_pixels)
{
    ImportFixture fixture;
    TextureConverter converter;
    converter.setOutputDir(fixture.output().string());
    TextureData image;
    image.name="strip";image.width=1;image.height=3;
    image.imageData={255,0,0,255, 0,255,0,255, 0,0,255,255};
    const auto first=converter.convertAll({image},"batch");
    CHECK(first.size()==1u);
    if(first.empty())return;
    TextureLoader loader;
    const auto loaded=std::dynamic_pointer_cast<Texture>(loader.load((fixture.output()/first[0].path).string()));
    CHECK(loaded!=nullptr);
    if(loaded){
        CHECK(loaded->getMipmapCount()==2u);
        const auto* mip=loaded->getMipmapData(1);
        CHECK(mip && mip[0]==127 && mip[1]==127 && mip[2]==0 && mip[3]==255);
    }
    image.imageData[0]=0;
    const auto changed=converter.convertAll({image},"batch");
    CHECK(changed.size()==1u);
    const auto updated=std::dynamic_pointer_cast<Texture>(loader.load((fixture.output()/first[0].path).string()));
    CHECK(updated!=nullptr);
    if(updated)CHECK(updated->getImageDataBytes()[0]==0);
}


TEST_CASE(gltf_auto_basis_and_unit_override_follow_the_source_standard)
{
    ImportFixture fixture;
    fixture.triangle(2.0f);
    auto options=fixture.options();
    const auto imported=importAsset(options);
    CHECK_MSG(imported.ok,imported.error.c_str());
    if(!imported.ok)return;
    MeshLoader loader;
    const auto mesh=std::dynamic_pointer_cast<Mesh>(loader.load((fixture.output()/fixture.meshPath(imported)).string()));
    CHECK(mesh!=nullptr);
    if(mesh){
        CHECK(std::abs(mesh->getBounds().getMin().x+2.0f)<.0001f);
        CHECK(std::abs(mesh->getBounds().getMax().y-2.0f)<.0001f);
    }
    options.sourceCoordinates.metersPerUnit=.25f;
    // Auto basis is format-defined; dormant manual axis fields must not leak.
    options.sourceCoordinates.up=ImportAxis::PositiveX;
    options.sourceCoordinates.forward=ImportAxis::NegativeY;
    options.sourceCoordinates.handedness=ImportHandedness::Left;
    const auto scaled=importAsset(options);
    CHECK_MSG(scaled.ok,scaled.error.c_str());
    const auto scaledMesh=std::dynamic_pointer_cast<Mesh>(loader.load((fixture.output()/fixture.meshPath(scaled)).string()));
    CHECK(scaledMesh!=nullptr);
    if(scaledMesh){
        CHECK(std::abs(scaledMesh->getBounds().getMin().x+.5f)<.0001f);
        CHECK(std::abs(scaledMesh->getBounds().getMax().y-.5f)<.0001f);
    }
}


TEST_CASE(scene_publication_refuses_missing_dependencies_and_duplicate_outputs)
{
    ImportFixture fixture;
    fixture.write(fixture.output()/"meshes/Unit.msh","last good mesh");
    detail::ImportOutputTransaction transaction(fixture.output().string());
    fixture.write(fs::u8path(transaction.stagingRoot())/"meshes/Unit.msh","new mesh bytes");
    ConversionResult conversion;
    ConversionResult::ConvertedResource mesh;
    mesh.path="meshes/Unit.msh";mesh.type="Mesh";mesh.size=14;
    conversion.resources.push_back(mesh);
    conversion.dependencies.push_back({mesh.path,"materials/Missing.mat"});
    bool rejected=false;
    try{transaction.validate(conversion,true);}catch(const std::exception&){rejected=true;}
    CHECK(rejected);
    CHECK(fixture.read(fixture.output()/"meshes/Unit.msh")=="last good mesh");
    conversion.dependencies.clear();
    conversion.resources.push_back(mesh);
    rejected=false;
    try{transaction.validate(conversion,true);}catch(const std::exception&){rejected=true;}
    CHECK(rejected);
}


TEST_CASE(corrupt_raw_image_import_does_not_replace_last_good_texture)
{
    ImportFixture fixture;
    const std::string hex="89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    std::string png;
    for(std::size_t i=0;i<hex.size();i+=2)png.push_back(char(std::stoi(hex.substr(i,2),nullptr,16)));
    const auto source=fixture.root/"Source/Surface.png";
    fixture.write(source,png);
    auto options=fixture.options();options.sourcePath=source.string();options.outputNamespace="raw-image";
    const auto first=importAsset(options);
    CHECK_MSG(first.ok,first.error.c_str());
    if(!first.ok)return;
    const auto path=fixture.output()/first.conversion.resources[0].path;
    const auto original=fixture.read(path),metadata=fixture.read(fs::u8path(first.depSidecarPath));
    fixture.write(source,"a damaged but nonempty PNG");
    const auto failed=importAsset(options);
    CHECK_FALSE(failed.ok);
    CHECK(fixture.read(path)==original);
    CHECK(fixture.read(fs::u8path(first.depSidecarPath))==metadata);
}

TEST_CASE(relinked_model_keeps_stable_output_stem_and_dependency_identity)
{
    ImportFixture fixture;
    fixture.triangle();
    auto options=fixture.options();
    options.sourceId="source-relink";
    options.outputNamespace="relink";
    options.outputBaseName="Original Triangle";
    const auto first=importAsset(options);
    CHECK_MSG(first.ok,first.error.c_str());
    if(!first.ok)return;
    const auto firstPath=fixture.meshPath(first);
    const auto before=fixture.read(fixture.output()/firstPath);
    const auto relocated=fixture.root/"Moved/Renamed.gltf";
    fixture.write(relocated,fixture.read(fixture.source()));
    fixture.write(relocated.parent_path()/"Triangle.bin",fixture.read(fixture.source().parent_path()/"Triangle.bin"));
    fs::remove(fixture.source());
    options.sourcePath=relocated.string();
    const auto second=importAsset(options);
    CHECK_MSG(second.ok,second.error.c_str());
    CHECK_FALSE(second.usedCache);
    CHECK(fixture.meshPath(second)==firstPath);
    CHECK(fixture.read(fixture.output()/firstPath)==before);
    CHECK(second.depSidecarPath==first.depSidecarPath);
    ImportRecord record;
    CHECK(readImportRecord(second.depSidecarPath,record));
    CHECK(record.options.outputBaseName=="Original Triangle");
    CHECK(record.options.sourceId==options.sourceId);
    CHECK(isImportRecordCurrent(record));
    CHECK(importAsset(options).usedCache);
    const auto metadata=fixture.read(fs::u8path(second.depSidecarPath));
    options.outputBaseName="../escape";
    CHECK_FALSE(importAsset(options).ok);
    CHECK(fixture.read(fs::u8path(second.depSidecarPath))==metadata);
}

TEST_CASE(relinked_standalone_image_keeps_existing_texture_reference)
{
    ImportFixture fixture;
    const std::string hex="89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    std::string png;
    for(std::size_t i=0;i<hex.size();i+=2)png.push_back(char(std::stoi(hex.substr(i,2),nullptr,16)));
    const auto source=fixture.root/"Source/Surface.png";
    fixture.write(source,png);
    auto options=fixture.options();options.sourcePath=source.string();
    options.sourceId="source-image-relink";options.outputBaseName="Surface";
    const auto first=importAsset(options);
    CHECK_MSG(first.ok,first.error.c_str());
    if(!first.ok)return;
    const auto moved=fixture.root/"Moved/Renamed.png";
    fixture.write(moved,png);fs::remove(source);
    options.sourcePath=moved.string();
    const auto second=importAsset(options);
    CHECK_MSG(second.ok,second.error.c_str());
    if(!second.ok)return;
    CHECK(second.conversion.resources[0].path==first.conversion.resources[0].path);
    CHECK(second.depSidecarPath==first.depSidecarPath);
    CHECK(importAsset(options).usedCache);
}

TEST_SUITE_END
