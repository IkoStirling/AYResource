#pragma once
#include "AYResource/ProjectBuild.h"
namespace ayt::resource::detail
{
void discoverProjectLogia(ProjectBuildPlan &plan);
void validateProjectLogia(const ProjectBuildPlan &plan,
                          std::vector<ProjectBuildDiagnostic> &diagnostics);
} // namespace ayt::resource::detail
