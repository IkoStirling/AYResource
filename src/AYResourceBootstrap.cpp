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

#include <memory>

namespace ayt::resource
{

namespace {

bool g_loadersInitialized = false;

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
    if (g_loadersInitialized) {
        return true;
    }

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

    g_loadersInitialized = ok;
    return ok;
}

bool areLoadersInitialized()
{
    return g_loadersInitialized;
}

} // namespace ayt::resource
