#pragma once
// AYResource.h - umbrella header

#include "interface/IAYResource.h"
#include "interface/assetsDefs/IAYMesh.h"
#include "interface/assetsDefs/IAYTexture.h"
#include "interface/assetsDefs/IAYMaterial.h"
#include "interface/assetsDefs/IAYAnimation.h"
#include "interface/assetsDefs/IAYAudio.h"
#include "interface/assetsDefs/IAYVideo.h"
#include "interface/assetsDefs/IAYFontAsset.h"
#include "interface/assetsDefs/IAYScript.h"
#include "interface/assetsDefs/IAYPhysics.h"
#include "interface/assetsDefs/IAYTilemap.h"
#include "interface/assetsDefs/IAYSkeleton.h"
#include "interface/IAYConverter.h"
#include "interface/IAYResourceLoader.h"
#include "AYResourceHandle.h"
#include "AYResourceManager.h"
#include "AYResourceRegistry.h"
#include "AYResourceCache.h"
#include "AYAsyncLoader.h"
#include "AYHotReloadWatcher.h"
#include "AYResourceBootstrap.h"
#include "AYAssetPath.h"

namespace ayt::resource
{

// Module version
constexpr int MAJOR_VERSION = 1;
constexpr int MINOR_VERSION = 0;
constexpr int PATCH_VERSION = 0;

} // namespace ayt::resource