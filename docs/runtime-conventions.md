# AYResource Runtime Conventions (R2b)

This document defines how AYResource CPU assets connect to AYShader (Phoskia) and
AYRenderer (GPU). Offline converters and runtime loaders must follow these rules.

## 1. Shader reference in `.aymat`

| Field | Value |
|-------|--------|
| `shader` | Virtual path to a **Phoskia source file**, e.g. `shaders/pbr.phoskia` |
| Not used at runtime | `.ayshader` (legacy GLSL/HLSL blob), `"Standard"` string ids |

### Phoskia → compiled GPU shader chain

```
aymat.shader path
  → resolveAssetPath(aymatDir, path)
  → AYIO File::readAllText(phoskiaPath)
  → ShaderResourcePool::acquire(source, cacheKey = phoskiaPath)
       → Phoskia compile → BGFX .sc → shaderc → .bin → bgfx::createShader
       → disk cache .aysc (AYShader internal, not an AYResource asset)
```

Bridge (R2b) reads Phoskia text; AYShader owns compile/cache/hot-reload.

## 2. Material parameter / texture names

Names must match Phoskia property / uniform / texture declarations.

| aymat param | Phoskia | Notes |
|-------------|---------|-------|
| `baseColor`, `albedo` | property / uniform | vec4 tint |
| `metallic`, `roughness`, `smoothness` | property | float |
| `albedoMap` | `texture2d albedoMap` | path in aymat → `.aytex` or image |
| `normalMap` | `texture2d normalMap` | optional |

## 3. Path resolution

```
resolveAssetPath(basePath, refPath):
  - refPath absolute → use as-is
  - else join directory(basePath) with refPath, normalize
  - optional global asset root (setAssetRoot) for bare refs
```

Examples:

- mesh slot `materials/hero.aymat` relative to `meshes/hero.aymesh` directory
- material texture `textures/albedo.aytex` relative to `materials/hero.aymat` directory
- phoskia `shaders/pbr.phoskia` relative to aymat or asset root

## 4. Mesh layout → renderer

| MeshAttribute bit | VertexLayoutDesc |
|-------------------|------------------|
| Position | `VertexAttribute::Position`, 3× float |
| Normal | `VertexAttribute::Normal`, 3× float |
| UV | `VertexAttribute::TexCoord0`, 2× float |
| Color | (R2b+ preset TBD) |
| Tangent | (R2b+ preset TBD) |

## 5. Index buffer policy (Scheme A)

| Layer | Format |
|-------|--------|
| `.aymesh` storage | always `uint32_t` indices |
| Bridge upload | if max index < 65536 → u16 IB; else u32 + `BGFX_BUFFER_INDEX32` |
| Renderer API | `createMesh` (u16) + `createMesh32` (u32) |

References: glTF UNSIGNED_SHORT/INT accessors; Unity 16/32-bit mesh indices; bgfx
`BGFX_BUFFER_INDEX32`.

## 6. Loading entry points

| API | Use |
|-----|-----|
| `initializeLoaders()` | Register all runtime loaders (call once at startup) |
| `ResourceRegistry::loadByPath(path)` | Debug, tests, loose files (no DB) |
| `ResourceManager::load<T>(path)` | Game/editor; DB + pak when registered; **falls back** to loose file |

## 7. Loose dependency sidecars + intrinsic L2 deps (P1)

Offline converters may emit `{assetStem}.aydep.json` next to an asset. At runtime,
`ResourceManager` loads dependencies listed for matching `from` paths before the
primary asset when using the loose-file path.

In addition (P1), after a primary asset parses successfully, Manager walks
**intrinsic** references and preloads them into the L2 cache:

| Owner | Intrinsic edges |
|-------|-----------------|
| `IMesh` | material slot path strings → `.aymat` |
| `IMaterial` | `Texture2D/3D/Cube` parameter paths → `.aytex` (or typed texture) |

Missing typed deps (`.aymat` / `.aytex`) are marked `ResourceLoadState::Failed` and
receive a placeholder in cache (default material / magenta 1×1 texture) so L3 bind
does not see a null. Primary load failure still returns `nullptr`.

## 7b. Hot reload (P2)

```
FileWatcher / mtime → HotReloadWatcher (debounce)
  → ResourceManager unload + _loadInternal          (L2)
  → setOnHotReload callback
       → RenderResourceManager.onResourceFileChanged (L3, stable ids)
```

- Successful loose loads auto-`watchResource` (disable via `setAutoWatchLoadedResources(false)`).
- Call `Renderer::pollResourceHotReload()` each frame (wired in `RendererSubSystem`).
- Shader source hot-reload remains `pollShaderHotReload()` (AYShader pool).

## 7c. Async + cache (P3)

| API | Behavior |
|-----|----------|
| `ResourceManager::loadAsync<T>` | Submits to `AYTask` default pool; typed future completes on worker callback |
| `ResourceCache` grace | After LRU demote / last `ResourceHandle` drop, short pin (`weakGraceSeconds`, default 2s) |
| `getCacheStats()` | `memoryBytes/Budget`, hits/misses/resurrects, strong/weak/grace counts |
| `save/loadPersistentCache` | Writes/reads `AYCACHE 1` residency index; load preloads listed paths |

## 7d. Ship cook / pak (P4)

| Piece | Location |
|-------|----------|
| Core API | `AYResource::cookShipPackage` (`AYResource/CookShip.h`) |
| CLI | `AYTool/cook_tool` → `--assets <cooked> --out <ship>` |
| Runtime | `ResourceManager::openDatabase("ship/resources.db")` |

Load order after `openDatabase`: **DB record → pak (`in_package`) → loose file fallback**.

## 7e. Import orchestration (P5)

| Piece | Location |
|-------|----------|
| Core API | `AYResource::importAsset` / `importAssetBatch` (`AYResource/ImportJob.h`) |
| CLI | `AYTool/import_tool` → `--in <src> --out <assetsDir>` |
| Editor | `ayt::editor::Importer::importFile` → thin wrapper (cache: `ayeditor_cache/assets\`) |

Progress via `ImportProgressFn`; cooperative cancel via `ImportCancelToken` (between stages).
`.aydep.json` sidecar enables cache reuse (same rules as Editor character import).

## 7f. Ownership / unload (P6)

Full contract: [`ownership-contracts.md`](ownership-contracts.md).

| Holder | Owns |
|--------|------|
| `ResourceCache` + `ResourceHandle` | L2 `shared_ptr<IResource>` |
| `RenderResourceManager` | L3 opaque GPU handles (no retained L2 ptr) |

`unload*` / `trimCache` → **L2 only** (no L3 notify). Hot-reload → `setOnHotReload` → Renderer `onResourceFileChanged` (stable ids).

## 8. Public include surface (Phase 3)

Consumers should include `AYResource.h` and link `AYResource`. The following are **PUBLIC**:

- `AYResource.h`, `interface/**`, `interface/AYResource/assetsDefs/**`
- `include/AYResource*.h`, `include/AYResource/AssetPath.h`, `include/AYResource/AsyncLoader.h`, `include/AYResource/HotReloadWatcher.h`, `include/AYResource/LooseDependency.h`, `include/AYResource/CookShip.h`, `include/AYResource/ImportJob.h`

The following are **PRIVATE** to the library and unit tests (not propagated to dependents):

- `include/AYResource/assetsImpl/**` — concrete asset classes (`Mesh`, `Material`, …)
- `include/AYResource/Loader/**`, `include/Converter/**` — loaders and offline converters

Legacy `.ayshader` / `ShaderLoader` / `ShaderConverter` remain for offline tooling only; they are **not** registered in `initializeLoaders()`. Prefer **not** to `#include "assetsImpl/..."` from engine code; use `interface/AYResource/assetsDefs/I*.h` instead.

Enforcement (P6): `AYTest_PublicApiSurface` scans `AYRuntime` production sources; exceptions live in [`private-include-allowlist.txt`](private-include-allowlist.txt).

## 9. Legacy

- `.ayshader` / `IShader`: offline or deprecated; not used in R2b render path.
- `MaterialFile` multi-material bundles: use `materialCount` + concatenated material blobs.

## 10. Phase 0 deferred items

See [`phase0-follow-ups.md`](phase0-follow-ups.md) for known issues intentionally
left out of Phase 0's exit criteria:

- **F-01**: `SubmeshData::vertexOffset` is dropped by `MeshConverter` (Phase 1 R-02 + AN-01 territory).
- **F-02**: `IMesh::Submesh` has no `#pragma pack` — future 8-byte fields will silently pad SUBM chunks.
- **F-03**: `_setForTest*` family is public on `Mesh` — should be demoted to a friend class or test-only namespace in Phase 1.

## 11. 纹理 dev 直引契约（Texture dev raw-reference mode）

开发期默认**不 cook 纹理**：FBX 导入时 `.aymat` 的纹理参数直接引用源图扩展名
（`textures/{stem}_d.png` / `_d.jpg` …），`TextureConverter` 把源文件**原样拷贝**
进 `textures/`；运行时 `TextureLoader` 用 stb_image 解码为单级 RGBA8 上传。
发布期才 cook 成 BC7+mips 的 `.aytex`。

### 开关与模式

| 入口 | 行为 |
|---|---|
| 默认（dev） | `ImportOptions.cookTextures=false` → raw 直引 |
| 发布 | `AY_IMPORT_COOK_TEXTURES=1`（编辑器）或 `import_tool --cook-textures` → BC7+mips cook，与旧行为一致 |
| 直接调用 `FBXConverter::convert()` | `_cookTextures` 默认 `true`，零翻转（保护既有调用方） |

`.aydep.json` 记录 `"textureMode": "raw" | "cook"`；**无该字段 = legacy 旧缓存**，
两种模式下都命中（旧 .aytex 缓存 dev 下照常渲染）。记录模式与请求模式不符 →
确定性缓存 miss（全量重导），绝不返回错模式的缓存。

### 路径契约（唯一拼写，见 `VirtualAssetPath.h`）

- 引用（FBXParser / dep 兜底）与输出（TextureConverter）共用
  `textures/{stem}{suffix}{ext}`；dev 下 `ext` = 源扩展名小写，**dds/aytex 源除外**
  （`textureDevExtensionOf` 强制 `.aytex` —— dds 走零解码 passthrough，防止悬空 .dds 引用）。
- 运行时 `TextureLoader` 的 loose 分支受编译宏 `AY_TEXTURE_LOOSE_FORMATS` 门控
  （镜像 `AY_AUDIO_LOOSE_FORMATS`；Debug/RelWithDebInfo 默认开，Release 关）。
  关闭时 `.png/.jpg/...` 不被注册/解析 —— `CookShip` 不会把 dev 残留图收进发布 pak。

### 规则

- 纹理源文件变更后必须 `AY_EDITOR_FORCE_IMPORT=1`（或 `--force`）重导：
  raw 拷贝的 SKIP 判定只比对文件大小，同尺寸不同内容不会自动重拷。
- GPU 上传只消费 mip 0（BGFXAdapter 单级）——dev 单级 RGBA8 与发布视觉行为一致；
  真 mipmap 是独立优化项。
- 嵌入式纹理（FBX 内嵌）无源文件可拷，继续 cook（规模小）。
- GLTF 导入路径不受影响（沿用既有 cook 行为）。
