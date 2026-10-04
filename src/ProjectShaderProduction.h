#pragma once
#include "AYResource/ProjectBuild.h"
#include <filesystem>
namespace ayt::resource::detail {
bool stageProductionShaders(const ProjectBuildPlan& plan,
    const std::filesystem::path& artifact,const std::filesystem::path& staging,
    const std::filesystem::path& content,std::vector<std::string>& packages,std::string& error);
}
