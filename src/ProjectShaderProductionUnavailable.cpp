#include "ProjectShaderProduction.h"
#include <AYAssetFormat/AssetFormat.h>
namespace ayt::resource::detail {
bool stageProductionShaders(const ProjectBuildPlan& plan,const std::filesystem::path& artifact,
    const std::filesystem::path&,const std::filesystem::path&,std::vector<std::string>&,std::string& error) {
    for(const auto& asset:plan.assets) if(ayt::asset_format::matchesPath(asset.sourcePath,ayt::asset_format::Id::PhoskiaShader)
        && asset.transform!=ProjectAssetTransform::Exclude) { error="Project shader build requires AYShader-enabled authoring tools";return false; }
    if(!artifact.empty() && std::filesystem::exists(artifact.parent_path()/"Shaders")) {
        error="Shader package validation requires AYShader-enabled authoring tools";return false;
    }
    return true;
}
}
