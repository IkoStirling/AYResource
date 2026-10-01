#include <AYResource/Converter/ParticleConverter.h>
#include <AYAssetFormat/AssetFormat.h>
#include <AYResource/Converter/TextureConverter.h>
#include <AYResource/LooseDependency.h>
#include <AYParticle/EffectPackaging.h>
#include <AYStorage/Guid.h>
#include <AYIO/File.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
namespace ayt::resource {
ConversionResult ParticleConverter::convert() {
    namespace fs=std::filesystem;
    particle::EffectSource source;
    std::string error;
    if(!particle::inspectEffectSource(_source,_root,source,&error)) throw std::runtime_error(error);
    ConversionResult result;
    result.textureMode=_cook?"cook":"raw";
    result.importerContractTag="particle-closure-rgba8-v3";
    std::map<fs::path,std::string> converted;
    for(size_t index=0; index<source.textures.size(); ++index) {
        const auto& texture=source.textures[index];
        if(texture.empty()) continue;
        auto found=converted.find(texture);
        if(found==converted.end()) {
            // Hash the entire relative filename, avoiding flattening collisions
            // and keeping cache paths short on Windows. Sharing is path-based.
            const auto relative=texture.lexically_relative(source.root).generic_string();
            const auto name="particle_"+storage::Guid::computeFromData(relative.data(),relative.size()).toString();
            TextureConverter converter;
            converter.setOutputDir(_output);
            converter.setUsageSuffix(""); converter.setCookTextures(_cook);
            // Soft transparent sprites benefit from lossless alpha over BC7.
            converter.setOutputFormat(TextureFormat::RGBA8);
            // TextureConverter may reuse same-size outputs. Always replace the
            // exact owned output to prevent stale standalone reimports.
            auto sourceExtension=texture.extension().string();
            std::transform(sourceExtension.begin(),sourceExtension.end(),sourceExtension.begin(),
                [](unsigned char value){ return static_cast<char>(std::tolower(value)); });
            const auto extension=(!_cook && sourceExtension!=".dds"
                && !ayt::asset_format::matchesPath(texture.string(), ayt::asset_format::Id::Texture))
                ? sourceExtension : std::string(ayt::asset_format::suffix(ayt::asset_format::Id::Texture));
            if(!_output.empty()) fs::remove(fs::path(_output)/"textures"/(name+extension));
            auto textures=converter.convertFromPath(texture.string(),name,texture.parent_path().string());
            if(textures.resources.size()!=1) throw std::runtime_error("Particle texture cook failed: "+relative);
            found=converted.emplace(texture,textures.resources.front().path).first;
            result.resources.insert(result.resources.end(),textures.resources.begin(),textures.resources.end());
            result.dependencies.push_back({source.logicalPath,found->second});
        }
        source.asset.emitters[index].effect.texturePath=found->second;
    }
    std::string json;
    if(!particle::encodeEffectAsset(source.asset,json,&error)) throw std::runtime_error(error);
    const auto path=fs::path(_output)/source.logicalPath;
    if(!_output.empty()) {
        fs::create_directories(path.parent_path());
        if(!io::File::atomicWrite(path.string(),json.data(),json.size())) throw std::runtime_error("Particle output write failed");
    }
    ConversionResult::ConvertedResource effect;
    effect.path=source.logicalPath; effect.type="ParticleEffect"; effect.size=json.size();
    effect.guid=storage::Guid::computeFromData(json.data(),json.size());
    result.resources.push_back(effect);
    if(!_output.empty()) {
        auto sidecar=result.toJson();
        if(!io::File::atomicWrite(looseDependencySidecarPath(path.string()),sidecar.data(),sidecar.size()))
            throw std::runtime_error("Particle dependency write failed");
    }
    return result;
}
}
