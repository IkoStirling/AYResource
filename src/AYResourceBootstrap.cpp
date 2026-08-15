#include "AYResource/ResourceBootstrap.h"

#include "AYResource/Loader/MeshLoader.h"
#include "AYResource/Loader/MaterialLoader.h"
#include "AYResource/Loader/TextureLoader.h"
#include "AYResource/Loader/SkeletonLoader.h"
#include "AYResource/Loader/SkeletonMaskLoader.h"
#include "AYResource/Loader/AnimationLoader.h"
#include "AYResource/Loader/AudioLoader.h"
#include "AYResource/Loader/VideoLoader.h"
#include "AYResource/Loader/FontLoader.h"
#include "AYResource/Loader/ScriptLoader.h"
#include "AYResource/Loader/PhysicsLoader.h"
#include "AYResource/Loader/TilemapLoader.h"
#include "AYResource/ResourceRegistry.h"

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
bool registerLoaderType(const char* typeName, const char* extension)
{
    ResourceRegistry::registerExtension(extension, typeName);
    return ResourceRegistry::registerLoader(typeName, &createLoaderInstance<LoaderT>);
}

} // namespace

bool initializeLoaders()
{
    std::call_once(g_loadersOnce, []() {
        const bool ok = registerLoaderType<MeshLoader>("Mesh", ".aymesh")
                     && registerLoaderType<MaterialLoader>("Material", ".aymat")
                     && registerLoaderType<TextureLoader>("Texture", ".aytex")
                     && registerLoaderType<SkeletonLoader>("Skeleton", ".ayskel")
                     && registerLoaderType<SkeletonMaskLoader>("SkeletonMask", ".aymask")
                     && registerLoaderType<AnimationLoader>("Animation", ".ayanm")
                     && registerLoaderType<AudioLoader>("Audio", ".ayaudio")
                     && registerLoaderType<VideoLoader>("Video", ".ayvideo")
                     && registerLoaderType<FontLoader>("Font", ".ayfont")
                     && registerLoaderType<ScriptLoader>("Script", ".ayscript")
                     && registerLoaderType<PhysicsLoader>("Physics", ".ayphys")
                     && registerLoaderType<TilemapLoader>("Tilemap", ".aytilemap");
#if defined(AY_AUDIO_LOOSE_FORMATS)
        // Dev/Editor: same Audio loader also owns loose authoring formats.
        ResourceRegistry::registerExtension(".wav", "Audio");
        ResourceRegistry::registerExtension(".mp3", "Audio");
        ResourceRegistry::registerExtension(".ogg", "Audio");
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
