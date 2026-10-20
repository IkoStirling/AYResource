#include <AYResource/ImportJob.h>
#include <AYIO/PathSafety.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace ayt::resource {
namespace {
namespace fs = std::filesystem;
std::string utf8(const fs::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string readText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || input.tellg() > 1024 * 1024)
        throw std::runtime_error("could not read bounded import recipe: " + utf8(path));
    input.seekg(0);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string resolve(const fs::path& project, const std::string& reference)
{
    if (reference.empty()) return {};
    if (reference == ".") return utf8(project);
    const auto path = fs::u8path(reference);
    if (path.is_absolute()) return utf8(path.lexically_normal());
    if (!ayt::io::path::isPortableRelative(reference))
        throw std::runtime_error("invalid project-relative import reference");
    fs::path resolved;
    std::string error;
    if (!ayt::io::path::resolveWithinRoot(project, project / path, resolved, error))
        throw std::runtime_error("invalid import recipe path: " + error);
    return utf8(resolved);
}
} // namespace
std::vector<ProjectImportRecipe> readProjectImportRecipes(const std::string& projectRoot, std::string* error)
{
    std::vector<ProjectImportRecipe> result;
    if (error) error->clear();
    try {
        const auto project = fs::weakly_canonical(fs::absolute(fs::u8path(projectRoot)));
        const auto directory = project / ".ayeditor/imports";
        if (!fs::is_directory(directory)) return result;
        for (const auto& file : fs::directory_iterator(directory)) {
            if (!file.is_regular_file() || file.is_symlink() || file.path().extension() != ".json") continue;
            const auto id = utf8(file.path().stem());
            if (id.rfind("source-", 0) != 0) continue;
            ProjectImportRecipe recipe;
            recipe.id = id;
            recipe.path = utf8(file.path());
            std::string parseError;
            if (!parseImportRecipe(readText(file.path()), recipe.options, &parseError)) {
                if (error) *error = "invalid import recipe " + id + ": " + parseError;
                continue;
            }
            recipe.options.sourcePath = resolve(project, recipe.options.sourcePath);
            recipe.options.outputDir = resolve(project, recipe.options.outputDir);
            recipe.options.sourceAssetRoot = resolve(project, recipe.options.sourceAssetRoot);
            if (recipe.options.sourceId.empty()) recipe.options.sourceId = recipe.id;
            result.push_back(std::move(recipe));
        }
        std::sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
            return first.id < second.id;
        });
    } catch (const std::exception& failure) { if (error) *error = failure.what(); }
    return result;
}

} // namespace ayt::resource
