#include "AYResourceBootstrap.h"

#include "Loader/MeshLoader.h"
#include "Loader/MaterialLoader.h"
#include "Loader/TextureLoader.h"
#include "Loader/SkeletonLoader.h"
#include "Loader/SkeletonMaskLoader.h"
#include "Loader/AnimationLoader.h"
#include "Loader/AudioLoader.h"
#include "Loader/VideoLoader.h"
#include "Loader/FontLoader.h"
#include "Loader/ScriptLoader.h"
#include "Loader/PhysicsLoader.h"
#include "Loader/TilemapLoader.h"
#include "AYResourceRegistry.h"

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
