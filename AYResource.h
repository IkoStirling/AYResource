#pragma once
// AYResource.h - umbrella header

#include "AYResource/IResource.h"
#include "AYResource/assetsDefs/IMesh.h"
#include "AYResource/assetsDefs/ITexture.h"
#include "AYResource/assetsDefs/IMaterial.h"
#include "AYResource/assetsDefs/IAnimation.h"
#include "AYResource/assetsDefs/IAudio.h"
#include "AYResource/assetsDefs/IVideo.h"
#include "AYResource/assetsDefs/IFontAsset.h"
#include "AYResource/assetsDefs/IScript.h"
#include "AYResource/assetsDefs/IPhysics.h"
#include "AYResource/assetsDefs/ITilemap.h"
#include "AYResource/assetsDefs/ISkeleton.h"
#include "AYResource/IConverter.h"
#include "AYResource/IResourceLoader.h"
#include "AYResource/ResourceHandle.h"
#include "AYResource/ResourceManager.h"
#include "AYResource/ResourceRegistry.h"
#include "AYResource/ResourceCache.h"
#include "AYResource/AsyncLoader.h"
#include "AYResource/HotReloadWatcher.h"
#include "AYResource/ResourceBootstrap.h"
#include "AYResource/AssetPath.h"

namespace ayt::resource
{

// Module version
constexpr int MAJOR_VERSION = 1;
constexpr int MINOR_VERSION = 0;
constexpr int PATCH_VERSION = 0;

} // namespace ayt::resource