#include "AYResource/ResourceBootstrap.h"

#include "AYResource/Loader/MeshLoader.h"
#include "AYResource/Loader/MaterialLoader.h"
#include "AYResource/Loader/TextureLoader.h"
#include "AYResource/Loader/SkeletonLoader.h"
#include "AYResource/Loader/SkeletonMaskLoader.h"
#include "AYResource/Loader/AnimationLoader.h"
#include "AYResource/Loader/AudioLoader.h"
#include <AYResource/AudioContent.h>
#include "AYResource/Loader/VideoLoader.h"
#include "AYResource/Loader/FontLoader.h"
#include "AYResource/Loader/ScriptLoader.h"
#include "AYResource/Loader/PhysicsLoader.h"
#include "AYResource/Loader/TilemapLoader.h"
#include "AYResource/Loader/AtlasLoader.h"
#include "AYResource/ResourceRegistry.h"
#include <AYAssetFormat/AssetFormat.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace ayt::resource
{

namespace {

// CM-5 (2026-08-12): init is now call_once-guarded. Before, a plain bool
// let every async-pool worker see `false` and run registerLoader*()
// concurrently → data race on the registry maps (heap corruption that
// surfaced as crashes in later tests, e.g. ResourceCache::clear).
std::atomic<bool> g_loadersInitialized{false};
std::once_flag g_loadersOnce;

template<typename LoaderT>
std::unique_ptr<IResourceLoader> createLoaderInstance()
{
    return std::make_unique<LoaderT>();
}

template<typename LoaderT>
bool registerLoaderType(const char* typeName, ayt::asset_format::Id format)
{
    ResourceRegistry::registerExtension(
        std::string(ayt::asset_format::suffix(format)), typeName);
    for (const auto alias : ayt::asset_format::describe(format).readAliases)
        ResourceRegistry::registerExtension(std::string(alias), typeName);
    return ResourceRegistry::registerLoader(typeName, &createLoaderInstance<LoaderT>);
}

} // namespace

bool initializeLoaders()
{
    std::call_once(g_loadersOnce, []() {
        using ayt::asset_format::Id;
        const bool ok = registerLoaderType<MeshLoader>("Mesh", Id::Mesh)
                     && registerLoaderType<MaterialLoader>("Material", Id::Material)
                     && registerLoaderType<TextureLoader>("Texture", Id::Texture)
                     && registerLoaderType<SkeletonLoader>("Skeleton", Id::Skeleton)
                     && registerLoaderType<SkeletonMaskLoader>("SkeletonMask", Id::SkeletonMask)
                     && registerLoaderType<AnimationLoader>("Animation", Id::Animation)
                     && registerLoaderType<AudioLoader>("Audio", Id::Audio)
                     && registerLoaderType<AudioContentLoader>("AudioContent", Id::AudioContent)
                     && registerLoaderType<VideoLoader>("Video", Id::Video)
                     && registerLoaderType<FontLoader>("Font", Id::Font)
                     && registerLoaderType<ScriptLoader>("Script", Id::ScriptBlob)
                     && registerLoaderType<PhysicsLoader>("Physics", Id::Physics)
                     && registerLoaderType<TilemapLoader>("Tilemap", Id::Tilemap)
                     && registerLoaderType<AtlasLoader>("Atlas", Id::Atlas);
#if defined(AY_AUDIO_LOOSE_FORMATS)
        // Dev/Editor: same Audio loader also owns loose authoring formats.
        ResourceRegistry::registerExtension(".wav", "Audio");
        ResourceRegistry::registerExtension(".mp3", "Audio");
        ResourceRegistry::registerExtension(".ogg", "Audio");
        // Metadata only: the optional AYParticleResources adapter owns its loader.
#if defined(AY_RESOURCE_HAS_PARTICLE)
        for (const auto& format : {Id::ParticleEffect}) {
            ResourceRegistry::registerExtension(std::string(ayt::asset_format::suffix(format)), "ParticleEffect");
            for (const auto alias : ayt::asset_format::describe(format).readAliases)
                ResourceRegistry::registerExtension(std::string(alias), "ParticleEffect");
        }
#endif
#endif
#if defined(AY_RESOURCE_HAS_STATS)
        ResourceRegistry::registerExtension(
            std::string(ayt::asset_format::suffix(Id::StatsRecipes)), "StatsRecipes");
        for (const auto alias : ayt::asset_format::describe(Id::StatsRecipes).readAliases)
            ResourceRegistry::registerExtension(std::string(alias), "StatsRecipes");
#endif
#if defined(AY_RESOURCE_HAS_DET_LEVEL)
        // Packaging metadata only; the optional AYEntity loader is explicitly registered by the host.
        const auto levelSuffix=std::string(ayt::asset_format::suffix(Id::DeterministicLevelCooked));
        if(ResourceRegistry::getTypeFromExtension(levelSuffix).empty())
            ResourceRegistry::registerExtension(levelSuffix, "DeterministicLevel2D");
#endif
#if defined(AY_TEXTURE_LOOSE_FORMATS)
        // Dev/Editor: same Texture loader also owns loose authoring formats
        // (dev raw-reference mode — ImportOptions.cookTextures=false copies
        // source png/jpg/... into textures/ and .aymat points at them).
        // Gated so release packs never collect stray .png via
        // CookShip::isCookableAsset (extension registry drives that scan).
        ResourceRegistry::registerExtension(".png", "Texture");
        ResourceRegistry::registerExtension(".jpg", "Texture");
        ResourceRegistry::registerExtension(".jpeg", "Texture");
        ResourceRegistry::registerExtension(".bmp", "Texture");
        ResourceRegistry::registerExtension(".tga", "Texture");
#endif
        g_loadersInitialized.store(ok);
    });
    return g_loadersInitialized.load();
}

bool areLoadersInitialized()
{
    return g_loadersInitialized.load();
}

} // namespace ayt::resource
