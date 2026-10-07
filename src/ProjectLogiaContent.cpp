#include "ProjectLogiaContent.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#if AY_RESOURCE_HAS_SCRIPT
#include <AYScript/logia/Logia.h>
#include <AYScript/logia/Modules.h>
#include <AYScript/logia/Parser.h>
#endif
namespace ayt::resource::detail
{
namespace
{
namespace fs = std::filesystem;
struct Dependencies { std::vector<std::string> sources, resources; };
using Closure = std::unordered_map<std::string, Dependencies>;
Closure inspect(const ProjectBuildPlan &plan, std::vector<ProjectBuildDiagnostic> &diagnostics)
{
    Closure closure;
    auto fail = [&](const std::string &path, const std::string &message) {
        diagnostics.push_back({ProjectBuildDiagnostic::Severity::Error, path, message});
    };
    std::error_code rootError;
    const auto root = fs::weakly_canonical(
        fs::path(plan.projectRoot) / plan.profile.content.assetRoot, rootError);
    if (rootError) {
        fail(plan.profile.content.assetRoot, "Logia asset root is unreadable: " + rootError.message());
        return closure;
    }
    std::unordered_map<std::string, const ProjectBuildAsset *> assets;
    for (const auto &asset : plan.assets)
        assets.emplace(asset.logicalPath, &asset);
    for (const auto &asset : plan.assets)
    {
        if (fs::path(asset.logicalPath).extension() != ".logia" ||
            asset.transform == ProjectAssetTransform::Exclude)
            continue;
        if (asset.transform != ProjectAssetTransform::Raw ||
            asset.storage != ProjectAssetStorage::Loose)
            fail(asset.logicalPath, "Logia sources require raw/loose publication.");
#if AY_RESOURCE_HAS_SCRIPT
        try
        {
            if (fs::file_size(asset.sourcePath) > 1024 * 1024)
            {
                fail(asset.logicalPath, "Logia entry exceeds 1 MiB.");
                continue;
            }
            std::ifstream file(asset.sourcePath, std::ios::binary);
            if (!file)
            {
                fail(asset.logicalPath, "Logia source is unreadable.");
                continue;
            }
            std::string source((std::istreambuf_iterator<char>(file)), {});
            std::vector<ayt::script::logia::Token> tokens;
            ayt::script::logia::tokenize(source, tokens);
            ayt::script::logia::Parser parser(tokens);
            auto program = parser.parse();
            for (const auto &error : parser.errors())
                fail(asset.logicalPath, "Logia " + std::to_string(error.line) + ":" +
                                            std::to_string(error.column) + ": " + error.message);
            if (parser.hasErrors() || !program)
                continue;
            auto context = ayt::script::logia::checkedLogiaHostContext();
            context.sourcePath = asset.sourcePath;
            context.assetRoot = root.generic_string();
            std::vector<std::string> dependencies, resources;
            for (const auto &diagnostic :
                 ayt::script::logia::expandLogiaModules(*program, context, dependencies, &resources))
                fail(asset.logicalPath, diagnostic.location.file + ":" +
                                            std::to_string(diagnostic.location.line) + ": " +
                                            diagnostic.message);
            auto &paths = closure[asset.logicalPath].sources;
            for (const auto &dependency : dependencies)
            {
                const auto relative = fs::relative(dependency, root).generic_string();
                auto found = assets.find(relative);
                if (found == assets.end() ||
                    found->second->transform == ProjectAssetTransform::Exclude)
                    fail(asset.logicalPath, "Missing or excluded Logia import: " + relative);
                else if (found->second->transform != ProjectAssetTransform::Raw ||
                         found->second->storage != ProjectAssetStorage::Loose)
                    fail(asset.logicalPath, "Logia import must be raw and loose: " + relative);
                paths.push_back(relative);
            }
            std::sort(paths.begin(), paths.end());
            auto& resourcePaths=closure[asset.logicalPath].resources;
            for(const auto& logical:resources) {
                auto found=assets.find(logical);
                if(found==assets.end() || found->second->transform==ProjectAssetTransform::Exclude)
                    fail(asset.logicalPath,"Missing or excluded declared Logia resource: "+logical);
                resourcePaths.push_back(logical);
            }
            std::sort(resourcePaths.begin(),resourcePaths.end());
        }
        catch (const std::exception &error)
        {
            fail(asset.logicalPath, std::string("Invalid Logia content: ") + error.what());
        }
#else
        fail(asset.logicalPath, "Logia publication requires AY_ENABLE_SCRIPT authoring support.");
#endif
    }
    return closure;
}
} // namespace
void discoverProjectLogia(ProjectBuildPlan &plan)
{
    const auto closure = inspect(plan, plan.diagnostics);
    for (auto &asset : plan.assets)
        if (auto found = closure.find(asset.logicalPath); found != closure.end())
        {
            asset.scriptDependencies = found->second.sources;
            asset.resourceDependencies = found->second.resources;
        }
}
void validateProjectLogia(const ProjectBuildPlan &plan,
                          std::vector<ProjectBuildDiagnostic> &diagnostics)
{
    const auto closure = inspect(plan, diagnostics);
    for (const auto &asset : plan.assets)
        if (auto found = closure.find(asset.logicalPath); found != closure.end())
            if (asset.scriptDependencies != found->second.sources ||
                asset.resourceDependencies != found->second.resources)
                diagnostics.push_back(
                    {ProjectBuildDiagnostic::Severity::Error, asset.logicalPath,
                     "Logia source/resource closure changed after planning; create a new build plan."});
}
} // namespace ayt::resource::detail
