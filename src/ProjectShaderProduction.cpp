#include "ProjectShaderProduction.h"
#include <AYShader/ShaderBuild.h>
#include <AYShader/ShaderAnalysis.h>
#include <AYShader/detail/PhoskiaSourceSnapshot.h>
#include <AYIO/File.h>
#include <AYIO/PathSafety.h>
#include <AYAssetFormat/AssetFormat.h>
#include <set>
namespace ayt::resource::detail {
bool stageProductionShaders(const ProjectBuildPlan& plan,
    const std::filesystem::path& artifact,const std::filesystem::path& staging,
    const std::filesystem::path& content,std::vector<std::string>& packages,std::string& error) {
    namespace fs=std::filesystem;
    try {
        bool hasSources=false;
        for(const auto& asset:plan.assets) if(ayt::asset_format::matchesPath(asset.sourcePath,ayt::asset_format::Id::PhoskiaShader)
            && asset.transform!=ProjectAssetTransform::Exclude) hasSources=true;
        const auto engineRoot=artifact.empty()?fs::path{}:artifact.parent_path()/"Shaders";
        shader::ShaderBuildReport engine;
        std::string platform=AY_PRODUCTION_SHADER_PLATFORM, profile=AY_PRODUCTION_SHADER_PROFILE;
        const auto expectedPlatform=platform=="osx"?"macos":platform;
        const auto requestedPlatform=plan.profile.platform=="osx"?"macos":plan.profile.platform;
        if((hasSources || (!engineRoot.empty() && fs::exists(engineRoot))) && expectedPlatform!=requestedPlatform)
            throw std::runtime_error("Project shader target differs from the configured native build target; cross-target shader publication is not implemented");
        if(!engineRoot.empty() && fs::exists(engineRoot)) {
            if(fs::is_symlink(engineRoot)) throw std::runtime_error("Symlink shader root rejected");
            size_t count=0;
            for(const auto& target:fs::directory_iterator(engineRoot)) {
                if(++count>16 || target.is_symlink() || !target.is_directory()) throw std::runtime_error("Invalid shader target directory");
                const auto path=target.path()/"engine";
                shader::ShaderBuildReport report;
                if(!shader::readShaderBuildPackage(path.string(),report)) {
                    error="Invalid engine shader package: ";for(const auto& e:report.errors) error+=e+"; ";return false;
                }
                if(target.path().filename()!=report.platform+"-"+report.profile) throw std::runtime_error("Shader directory/target mismatch");
                const auto relative=fs::path("Shaders")/target.path().filename()/"engine";
                std::vector<std::string> errors;
                if(!shader::writeShaderBuildPackage(report,(staging/relative).string(),errors)) {
                    error="Cannot stage engine shaders: ";for(const auto& e:errors) error+=e+"; ";return false;
                }
                packages.push_back(relative.generic_string());
                if(report.platform==platform && report.profile==profile) engine=std::move(report);
            }
        }
        if(!hasSources) return true;
        if(!engineRoot.empty() && fs::exists(engineRoot) && !engine.success)
            throw std::runtime_error("No engine shader package for configured project shader target");
        shader::ShaderBuildManifest manifest;
        manifest.target={AY_SHADER_SHADERC_HINT,platform,profile,{AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT}};
        manifest.variantBudget=256-(engine.success?engine.entries.size():0);
        std::vector<std::string> selected;
        for(const auto& asset:plan.assets) {
            if(!ayt::asset_format::matchesPath(asset.sourcePath,ayt::asset_format::Id::PhoskiaShader) || asset.transform==ProjectAssetTransform::Exclude) continue;
            if(asset.transform!=ProjectAssetTransform::Raw || asset.storage!=ProjectAssetStorage::Loose)
                throw std::runtime_error("Phoskia sources/modules must be raw/loose: "+asset.logicalPath);
            fs::path resolved;std::string pathError;
            if(!ayt::io::path::resolveWithinRoot(content,content/fs::u8path(asset.logicalPath),resolved,pathError))
                throw std::runtime_error(pathError);
            selected.push_back(resolved.string());
        }
        std::vector<std::string> collectionErrors;
        if(!shader::collectShaderBuildJobs(selected,manifest.jobs,collectionErrors)) {
            error="Project shader collection failed";for(const auto& e:collectionErrors) error+="; "+e;return false;
        }
        if(manifest.jobs.empty()) return true;
        shader::ShaderBuildSession session; shader::ShaderBuildReport report;
        session.build(manifest,report);
        if(!report.success) { error="Project shader build failed";for(const auto& e:report.errors) error+="; "+e;return false; }
        const auto relative=fs::path("Shaders")/(platform+"-"+profile)/"project";
        std::vector<std::string> errors;
        if(!shader::writeShaderBuildPackage(report,(staging/relative).string(),errors)) {
            error="Cannot publish project shaders";for(const auto& e:errors) error+="; "+e;return false;
        }
        packages.push_back(relative.generic_string());return true;
    } catch(const std::exception& e) { error=e.what();return false; }
}
}
