# Cross-module load / own / unload contracts (P6)

This is the binding contract for L2 (AYResource) vs L3 (AYRenderer) ownership.
See also [`runtime-conventions.md`](runtime-conventions.md) and [`pipeline-roadmap.md`](pipeline-roadmap.md).

## 1. Who holds what

| Layer | Type | Owner | Lifetime |
|-------|------|-------|----------|
| L2 CPU asset | `shared_ptr<IResource>` / `IMesh`… | `ResourceCache` (strong / grace / weak) + live `ResourceHandle<T>` pins | Until `unload*` / LRU demote + grace expiry, or last external `shared_ptr` drops |
| L2 accessor | `ResourceHandle<T>` | Caller (game / editor) | Registers path on cache; `get()` re-reads cache after hot-reload |
| L2 transient | `shared_ptr<T>` from `load<T>` | Caller | Keeps object alive even after cache remove until released |
| L3 GPU | `MeshHandle` / `MaterialHandle` / `TextureHandle` | `RenderResourceManager` maps | Until `destroy*` / `shutdown`, or path re-upload on hot-reload (**stable id**) |

**Rule:** L3 upload copies CPU data into GPU buffers. Renderer does **not** retain L2 `shared_ptr` after `loadMesh` / `loadMaterial` / `loadTexture` returns.

```
Game / ECS
  → ResourceManager::load<IMesh>(path)          // L2
  → Renderer::loadMesh(path)                    // L2 load + L3 upload; keep MeshHandle
  → draw with MeshHandle                        // never hold raw IMesh* across frames as ownership
```

## 2. Unload / trim vs hot-reload

| API | L2 effect | Notifies L3? | Notes |
|-----|-----------|--------------|-------|
| `unloadResource` / `unloadAll` / `unloadTagged*` | Remove from cache (strong/grace/weak for path) | **No** | Intentional: GPU may still draw last upload |
| `trimCache` | LRU demote under budget | **No** | May resurrect via weak/grace |
| File change → `_handleHotReload` | `unload` + eager `_loadInternal` + `setOnHotReload` | **Yes** (via callback) | Renderer installs `onResourceFileChanged` |
| `reloadResource` | In-place `IResource::reload` if cached | **No** | Prefer hot-reload path for disk edits |

**Cross-module notify convention:** the only automatic L2→L3 sync is **hot-reload**:

```
HotReloadWatcher → ResourceManager::_handleHotReload
  → setOnHotReload(path)
       → RenderResourceManager::onResourceFileChanged(path)  // stable handle ids
```

Frame pump: `Renderer::pollResourceHotReload()` → `ResourceManager::update`.

If gameplay needs L3 drop when L2 unloads, call Renderer `destroy*` explicitly — do not assume Manager unload tears down GPU.

## 3. Include boundary (game / ECS)

| May include | Must not include |
|-------------|------------------|
| `AYResource.h`, `interface/**`, `interface/assetsDefs/IAY*.h` | `include/assetsImpl/**` |
| Manager / Handle / Cache / AssetPath / Async / HotReload / Import / Cook headers | `include/Loader/**`, `include/Converter/**` |

Exceptions (documented debt / bridge):

- `AYResource` itself + its unit tests
- `AYRenderer/src/detail/**` (L2→L3 bridge; prefer `IAY*` queries)
- Demos / unit tests outside production `src/` / `include/`
- Allowlisted production files in [`private-include-allowlist.txt`](private-include-allowlist.txt) (must shrink over time)

Enforced by `unittest/AYTest_PublicApiSurface.cpp` (compile umbrella + scan).

## 4. Quick checklist for PRs

1. Gameplay loads via `ResourceManager::load` / `createHandle`, not `ResourceRegistry` (production).
2. Renderer keeps opaque L3 handles; no long-lived L2 ownership in RRM.
3. Do not expect `unload`/`trim` to free GPU — use destroy or wait for hot-reload.
4. New `#include "assetsImpl/..."` in production modules fails CI unless allowlisted with justification.
