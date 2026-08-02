# AYResource — Design (v2.0)

**Status:** Living document, aligned with [`ENGINE-FOUNDATION-PLAN.md`](../../../../ENGINE-FOUNDATION-PLAN.md) and [`docs/runtime-conventions.md`](docs/runtime-conventions.md)
**Last revised:** 2026-07-27
**Owner:** Content engineer

---

## 0. Reading guide

| Audience | Read |
|----------|------|
| All engine engineers | §1, §2 (boundaries), §6 (public API) |
| Content / importer owners | §3 (L1 disk formats), §4 (converters + IntermediateAsset), §5 (incl. §5.7 MMD/saba future frontend), §5 (loaders) |
| Renderer / shader owners | §7 (L2 → L3 bridge contract), `docs/runtime-conventions.md` |
| New engineers | §2 + §6 + linked `IAY*.h` headers |
| PM / sprint | §10 (Phase 0 backlog) |

Related docs:

- Engine plan: [`ENGINE-FOUNDATION-PLAN.md`](../../../../ENGINE-FOUNDATION-PLAN.md)
- Runtime conventions (L1 ↔ renderer): [`docs/runtime-conventions.md`](docs/runtime-conventions.md)
- Ownership / unload (P6): [`docs/ownership-contracts.md`](docs/ownership-contracts.md)
- Pipeline roadmap: [`docs/pipeline-roadmap.md`](docs/pipeline-roadmap.md)
- AYRenderer design: `AYRuntime/AYRenderer/design.md`
- AYShader design: `AYRuntime/AYShader/design.md`
- AYAnimation design: `AYRuntime/AYAnimation/design.md`
- AYEntity design: `AYRuntime/AYEntity/design.md`
- AYAudio design (runtime playback): `AYRuntime/AYAudio/design.md`

---

## 1. Mission and non-goals

### 1.1 Mission

`AYResource` owns **L1 content** (`.ay*` files on disk + offline converters) and **L2 runtime assets** (CPU-decoded `IResource` objects, cached, dependency-aware). It is the bridge between offline authoring tools (FBX / glTF / PNG / …) and the runtime engine.

### 1.2 Non-goals (enforced in code review)

| AYResource does NOT | Owner instead |
|---------------------|---------------|
| Per-frame bone math, skinning, or sampling | `AYAnimation` |
| Drawing, shader binding, `bgfx::*` references | `AYRenderer`, `AYShader` |
| Embed Assimp / FBX / PMX parsers inside runtime loaders | (converters only) |
| Hold GPU handles anywhere visible to gameplay code | `AYRenderer` |
| Act as scene / prefab format (entity graph, components) | `AYSerializer` (future scene module) |
| Own L3 (RenderMesh, GPU VB/IB/textures) | `AYRenderer` |

### 1.3 Goals (what "industrial-grade resource pipeline" means here)

1. **Format-agnostic content layer.** Same `.ay*` files serve FBX-, glTF-, PMX-, VMD-imported content. MMD is a frontend, not an architecture.
2. **Hot-reload friendly.** Asset edited on disk → next access sees new L2 object; no process restart.
3. **Async-by-default with cancellation and progress.** No main-thread stalls on real production assets.
4. **Dependency-aware.** Material references its textures by path; loader resolves them lazily. No global scan at startup.
5. **Loose file in dev, pak/DB in ship.** Same L2 interface either way.
6. **Stable L1 versions.** Loaders accept N-1; converters emit current N. Disk format changes go through `Extension` four-cc chunks first.
7. **Public API surface is narrow and stable.** Game / ECS / editor code includes `interface/assetsDefs/IAY*.h` only; concrete impls are private to this module.

---

## 2. Architectural position

```
                         Offline (build time)
   ┌──────────────────────────────────────────────────────────────┐
   │  FBX / glTF / PMX / PNG / OGG / …                            │
   │     ↓ Parser (FBXParser, …)                                   │
   │  IntermediateAsset { MeshData, MaterialData, TextureData,    │
   │                       SkeletonData, AnimationData, … }       │
   │     ↓ typed Converters (Mesh / Material / Texture / …)       │
   │  L1: .aymesh / .ayskel / .ayanm / .aymat / .aytex / …        │
   │  + .aydep.json sidecar (offline only)                        │
   └──────────────────────────────────────────────────────────────┘
                                │
                                │  Runtime load
                                ▼
   ┌──────────────────────────────────────────────────────────────┐
   │  AYResource                                                  │
   │  LoaderRegistry ──> IResourceLoader (per format)             │
   │     │                                                       │
   │     ▼                                                       │
   │  L2: IResource objects (IMesh, ISkeleton, IAnimation, …)    │
   │  cached in ResourceCache; ref-counted via ResourceHandle     │
   │  dependencies resolved lazily (AYAssetPath)                  │
   └──────────────────────────────────────────────────────────────┘
                                │
                                │  RenderAssetBridge (in AYRenderer)
                                ▼
   ┌──────────────────────────────────────────────────────────────┐
   │  AYRenderer — L3: RenderMesh / GPU textures / materials      │
   └──────────────────────────────────────────────────────────────┘
```

Three rules from `ENGINE-FOUNDATION-PLAN.md` §2.1, restated:

1. **AYResource exposes only paths and opaque handles** to the rest of the engine. Never raw `bgfx::*` and never raw `IMesh*` in gameplay code.
2. **L1 → L2 is one-way.** L2 objects are immutable post-load. Hot-reload creates a new L2 object.
3. **L2 → L3 goes through `RenderAssetBridge` in `AYRenderer`.** AYResource has zero knowledge of L3.

---

## 3. Module layout (current state)

```
AYResource/
├── AYResource.h                 # Umbrella header for consumers
├── CMakeLists.txt
├── design.md                    # This document
│
├── interface/                   # PUBLIC: contracts only
│   ├── IAYResource.h            # Base IResource
│   ├── IAYResourceLoader.h      # Loader contract
│   ├── IAYConverter.h           # Converter contract
│   └── assetsDefs/              # Per-asset L2 interfaces (PUBLIC)
│       ├── IAYMesh.h
│       ├── IAYSkeleton.h
│       ├── IAYAnimation.h
│       ├── IAYMaterial.h
│       ├── IAYTexture.h
│       ├── IAYAudio.h
│       ├── IAYVideo.h
│       ├── IAYFontAsset.h
│       ├── IAYShader.h          # legacy; offline tooling only
│       ├── IAYPhysics.h
│       └── IAYScript.h
│
├── include/                     # PUBLIC headers + PRIVATE impl
│   ├── AYResourceManager.h      # PUBLIC: cache + async load
│   ├── AYResourceCache.h        # PUBLIC: weak/strong cache
│   ├── AYResourceHandle.h       # PUBLIC: ref-counted handle
│   ├── AYResourceRegistry.h     # PUBLIC: loader routing
│   ├── AYResourceBootstrap.h    # PUBLIC: initializeLoaders()
│   ├── AYAsyncLoader.h          # PUBLIC: real async + progress + cancel
│   ├── AYAssetPath.h            # PUBLIC: path resolver (dev/asset root)
│   ├── AYHotReloadWatcher.h     # PUBLIC: file-watcher for hot-reload
│   ├── AYLooseDependency.h      # PUBLIC: .aydep.json loose-side loader
│   ├── AYIntermediateAsset.h    # PUBLIC-ish: shared between Conv + tests
│   ├── assetsImpl/              # PRIVATE: concrete IResource classes
│   │   ├── AYMesh.h / .cpp
│   │   ├── AYSkeleton.h / .cpp
│   │   ├── AYAnimation.h / .cpp
│   │   ├── AYMaterial.h / .cpp
│   │   ├── AYTexture.h / .cpp
│   │   ├── AYAudio.h / .cpp
│   │   ├── AYVideo.h / .cpp
│   │   ├── AYFontAsset.h / .cpp
│   │   ├── AYShader.h / .cpp
│   │   ├── AYPhysics.h / .cpp
│   │   └── AYScript.h / .cpp
│   ├── Loader/                  # PRIVATE: per-format loaders
│   │   ├── MeshLoader.h / .cpp
│   │   ├── SkeletonLoader.h / .cpp
│   │   ├── AnimationLoader.h / .cpp
│   │   ├── MaterialLoader.h / .cpp
│   │   ├── TextureLoader.h / .cpp
│   │   ├── AudioLoader.h / .cpp
│   │   ├── VideoLoader.h / .cpp
│   │   ├── FontLoader.h / .cpp
│   │   ├── ShaderLoader.h / .cpp       # legacy
│   │   ├── ScriptLoader.h / .cpp
│   │   ├── PhysicsLoader.h / .cpp
│   │   └── MaterialFile.h / .cpp        # multi-material bundle helper
│   └── Converter/               # PRIVATE: offline converters
│       ├── FBXConverter.h / .cpp
│       ├── FBXParser.h / .cpp
│       ├── GLTFConverter.h / .cpp       # stub today
│       ├── MeshConverter.h / .cpp
│       ├── SkeletonConverter.h / .cpp
│       ├── AnimationConverter.h / .cpp
│       ├── MaterialConverter.h / .cpp
│       ├── TextureConverter.h / .cpp
│       ├── ITextureCompressor.h         # BC1/BC3/BC5/BC7 abstraction
│       ├── TextureCompressor.h / .cpp   # factory
│       ├── AudioConverter.h / .cpp
│       ├── VideoConverter.h / .cpp
│       ├── FontConverter.h / .cpp
│       └── ShaderConverter.h / .cpp     # legacy
│
├── src/                         # Implementations of public include/*
│   ├── AYResourceManager.cpp
│   ├── AYResourceCache.cpp
│   ├── AYResourceRegistry.cpp
│   ├── AYResourceBootstrap.cpp
│   ├── AYAssetPath.cpp
│   ├── AYHotReloadWatcher.cpp
│   ├── AYLooseDependency.cpp
│   ├── IAYConverter.cpp                 # factory + shared logic
│   ├── AssetsImpl/                      # mirrors include/assetsImpl/
│   ├── Loader/                          # mirrors include/Loader/
│   └── Converter/                       # mirrors include/Converter/
│
├── docs/
│   └── runtime-conventions.md    # L1 ↔ renderer contract (PUBLIC)
│
└── unittest/                    # ~15 unit tests; see §11
```

---

## 4. L1 — Disk format policy

### 4.1 Principle

Per `ENGINE-FOUNDATION-PLAN.md` §2.3, **prefer extension over rewrite.** A format change should land as a new `Extension` four-cc chunk before spawning a new file type.

### 4.2 Formats and current state

| Format | Magic | Version | Status today | Phase 0–1 plan |
|--------|-------|---------|--------------|----------------|
| `.aymesh` | `'AYMH'` | v1 | Static mesh + optional skin weights (`SkinWeight` bit `1<<5`) + `Extension[]` | **Phase 0:** stabilize skin-weights upload path. **v2 optional:** explicit `MORP` and `SKEL` chunks (use `IMesh::Extension` first) |
| `.ayskel` | `'AYSK'` | v1 | Bone hierarchy + bind pose + inverse bind matrix | Stable; bone-name hash optional |
| `.ayanm` | `'AYNM'` | v1 | Node-name-keyed tracks (pos/rot/scale) | Stable; ensure skeleton-binding metadata (skel path + bone-name table) |
| `.aymat` | `'AYMT'` | v1 | Phoskia shader path + typed params + texture slots | Stable; shader variant tags (`skinned`, `morph`) as params, not new file type |
| `.aytex` | — | v1 | Pixel data + mipmaps | Stable |
| `.ayfont` | — | v1 | Glyph atlas | Stable |
| `.ayaudio` | — | v1 | PCM/OGG/OPUS | Stable; format choice policy in `IAYAudio` |
| `.ayvideo` | — | v1 | RGBA frames | Stable |
| `.ayscript` | — | v1 | Source text | Stable |
| `.ayphys` | — | v1 | Shape parameters | Stable |
| `.ayshader` | — | v1 | **Legacy** GLSL/HLSL blob | **Deprecated for runtime.** Phoskia is the new shader source format. See `runtime-conventions.md` §1. |

### 4.3 `.aymesh` binary layout (L1 v1)

Storage is **always** `uint32_t` indices; the u16/u32 split happens at upload in `AYRenderer` (see `runtime-conventions.md` §5).

```
Header:
    uint32_t magic          = 'AYMH'
    uint16_t version        = 1
    uint8_t  attributeMask  (MeshAttribute bits; see §7.1)
    uint8_t  flags          (reserved)
    uint32_t vertexCount
    uint32_t vertexStride   (bytes per vertex, interleaved)
    uint32_t indexCount
    uint32_t submeshCount
    uint32_t materialSlotCount
    float    boundsMin[3]
    float    boundsMax[3]
    uint8_t  hasBounds
    uint8_t  reserved[3]

MaterialSlots[materialSlotCount]:
    uint32_t pathLength
    char     path[pathLength]                // virtual path, e.g. "materials/Skin.aymat"

Vertices[vertexCount * vertexStride]:
    Interleaved: position (12B), normal (12B, optional),
                 uv (8B, optional), tangent (16B, optional),
                 color (16B, optional),
                 skinWeight (24B, optional: 4×u8 indices + 4×f32 weights)

Indices[indexCount]:
    uint32_t indices[]

Submeshes[submeshCount]:
    uint32_t indexOffset
    uint32_t indexCount
    uint32_t materialIndex                  // identifier into .aydep.json

Extensions[] (optional, four-cc):
    uint32_t type        ('MORP' | 'CLTH' | 'PHYS' | 'SKEL' | user-defined)
    uint32_t size
    uint8_t  data[size]
```

Version policy: loaders accept v1 and **at most** v(n-1); converters emit current v(n).

### 4.4 `.aydep.json` (offline sidecar)

Generated by converters; describes submesh → material mapping. Schema in old design §5.2.1 is unchanged. **Runtime must function with this file absent** — its only role is offline importer bookkeeping and loose-file dependency hint (see §6.2).

---

## 5. Converters and IntermediateAsset

### 5.1 Architecture: Parser → IntermediateAsset → typed Converters

This split is the standard engine pattern (Unity, Unreal, Godot all do this). It lets us add a new source format without rewriting per-type converters.

```
source.fbx
   │
   ▼
FBXParser → IntermediateAsset
   │
   ├──> MeshConverter        ──> .aymesh
   ├──> SkeletonConverter    ──> .ayskel
   ├──> AnimationConverter   ──> .ayanm          (Phase 1: R-02)
   ├──> MaterialConverter    ──> .aymat
   └──> TextureConverter     ──> .aytex (BC3/BC7) or copy PNG
   │
   ▼
.aydep.json
```

`IntermediateAsset` (in `include/AYIntermediateAsset.h`) is the format-neutral in-memory representation:

```cpp
struct MeshData       { name, positions, normals, uvs, tangents, colors,
                        indices, submeshes, materialSlots, attributeMask,
                        skinWeights (Phase 1+); };
struct MaterialData   { name, shaderPath (Phoskia), params, textureRefs };
struct TextureData    { name, width, height, format, imageData, usage };
struct SkeletonData   { name, bones[] };
struct AnimationData  { name, duration, ticksPerSecond, tracks[] };
struct IntermediateAsset {
    std::vector<MeshData>       meshes;
    std::vector<MaterialData>   materials;
    std::vector<TextureData>    textures;
    std::vector<SkeletonData>   skeletons;     // Phase 1: R-03
    std::vector<AnimationData>  animations;    // Phase 1: R-02
    std::vector<AudioData>      audios;
};
```

### 5.2 IConverter contract

`interface/IAYConverter.h`:

```cpp
class IConverter {
public:
    virtual ~IConverter() = default;

    virtual void setSourcePath(const std::string&) = 0;
    virtual void setOutputDir (const std::string&) = 0;
    virtual void setLoadOption(LoadOption)        = 0;   // Full | MeshOnly

    virtual ConversionResult convert() = 0;
    virtual bool isValid() const = 0;
    virtual const char* getSourceType() const = 0;       // "FBX" | "glTF" | …

    static std::unique_ptr<IConverter> create(const std::string& sourcePath);
};

enum class LoadOption { Full, MeshOnly };

struct ConvertedResource { FGuid guid; std::string path; std::string type; int64_t size; };
struct Dependency        { std::string from; std::string to; };
struct ConversionResult  {
    std::vector<ConvertedResource> resources;
    std::vector<Dependency>        dependencies;
    std::string toJson() const;
};
```

### 5.3 Required converter behavior

Required of every concrete converter (`MeshConverter`, `MaterialConverter`, `TextureConverter`, `AnimationConverter`, `AudioConverter`, `VideoConverter`, `FontConverter`, `SkeletonConverter`):

| Capability | Why |
|------------|-----|
| **Atomic write per resource** (`write to tmp` → `rename`) | Crash mid-run must not corrupt prior outputs |
| **Skip-on-exists with size sanity** | Restartable; format change ⇒ size delta ⇒ re-emit |
| **`.aydep.json` emit** | Offline importer bookkeeping |
| **GUID from content** (`SHA256(content)`) | Stable identity across rebuilds |

### 5.4 Texture conversion policy

`TextureConverter` is the most complex converter — see old design §5.9 for full spec. Summary:

| Mode | Pre-process | Output | Runtime loader behavior |
|------|-------------|--------|-------------------------|
| **Preprocess = true** | BC3 / BC7 compress | `.aytex` | Direct GPU upload |
| **Preprocess = false** | Copy as-is | `.png` / `.jpg` | CPU decode → GPU upload at load |

`ITextureCompressor` is the abstraction over `squish` (BC1/BC3/BC5) and `Basis Universal` (BC7). Used offline only.

### 5.5 Material → Phoskia shader reference

`MaterialData::shader` is the **virtual path to a Phoskia source file** (e.g. `shaders/pbr.phoskia`). The runtime bridge (`runtime-conventions.md` §1) reads the Phoskia text and hands it to `AYShader::ShaderResourcePool`. Material assets do **not** embed compiled shader binaries.

### 5.6 Status of source-format coverage

| Source | Parser | Mesh | Skeleton | Animation | Material | Texture | Notes |
|--------|--------|------|----------|-----------|----------|---------|-------|
| FBX | ✅ | ✅ | ✅ | ✅ (R-02 wired) | ✅ | ✅ | Primary path today |
| glTF 2.0 | 🔄 stub | 🔄 | 🔄 | 🔄 | 🔄 | 🔄 | R-04 in Phase 1 |
| PMX / VMD (MMD) | — | — | — | — | — | — | **Future frontend** — §5.7 (saba); not started |
| PNG/JPG/BMP/TGA | n/a | n/a | n/a | n/a | n/a | ✅ | |
| OGG/MP3/PCM | n/a | n/a | n/a | n/a | n/a | n/a | ✅ |

### 5.7 Future extension: MMD (PMX / VMD) via saba（可扩展方向，未实现）

> **状态（2026-07-27）**：设计锁定为 **可选离线前端**；**不**进入当前开发进程。近期「看见动画」继续用 **Blender mmd_tools → FBX → `FBXConverter`**。  
> **目标**：批量 / 无人值守时，直接 `PMX`+`VMD` → 引擎 L1（`.ayskel` / `.ayanm` / `.aymesh`…），**不改** runtime `ISkeleton` / `IAnimation` / `AnimationPlayer`。

#### 5.7.1 Why this fits the existing pipeline

§5.1 already splits **Parser (source format)** from **typed Converters (engine assets)**. MMD is another Parser frontend — same pattern as FBX:

```
PMX (+ optional VMD)
   │
   ▼
SabaParser / MMDParser   ← wraps third-party saba (PMD/PMX/VMD)
   │
   ▼
IntermediateAsset        ← same structs as FBX path
   │
   ├──> MeshConverter        ──> .aymesh
   ├──> SkeletonConverter    ──> .ayskel
   ├──> AnimationConverter   ──> .ayanm
   ├──> MaterialConverter    ──> .aymat   (subset / bake; Toon optional later)
   └──> TextureConverter     ──> .aytex / copy

Facade: MMDConverter : IConverter   (getSourceType() == "MMD" | "PMX")
```

| Layer | Owns | Does NOT own |
|-------|------|----------------|
| **saba** (optional dep) | Read PMX/PMD/VMD; optional offline IK bake | Runtime playback |
| **SabaParser** | Fill `IntermediateAsset` (bone names, tracks, mesh, IBM) | `.ay*` binary layout |
| **Existing Converters** | Emit L1 + GUID + `.aydep.json` | MMD file formats |
| **AYAnimation** | Sample `.ayanm` by bone **name** | MMD / saba types |

**Invariant:** L1 remains format-agnostic (§1.3). Day-one FBX and future MMD produce the **same** `.ayskel`/`.ayanm` contracts. Runtime never `#include`s saba.

#### 5.7.2 Library choice

| Option | Role |
|--------|------|
| **[saba](https://github.com/benikabocha/saba)** (preferred) | C++ PMD/PMX/VMD load + solve; MIT; common in MMD viewers |
| MMDFormats / vmdio | Parse-only alternatives if saba is too heavy |
| Assimp | **Not** a reliable MMD frontend (incomplete / unused MMD paths) |
| Blender mmd_tools | Near-term **manual** path; remains valid forever as content toolchain |

CMake: `AY_RESOURCE_USE_SABA` (OFF by default). When OFF, `MMDConverter` is not built; no link to saba.

#### 5.7.3 Conversion contracts (when implemented)

1. **Paired import:** VMD bone names match the **PMX** Japanese names. API shape:
   - `MMDConverter::setModelPath(pmx)` + `setMotionPath(vmd)` or `convert(pmx, vmd)`.
   - Motion-only without a model is unsupported (or requires an explicit prior `.ayskel` + name table).
2. **Bone names on disk:** First ship may **keep Japanese standard names** in `.ayskel`/`.ayanm` (Player binds by string — works). Optional later: map to engine Humanoid canonical names (see `AYAnimation` Retarget / Humanoid table) **inside the Parser**, never at tick time.
3. **IK / physics / morph (v1):** Prefer **bake into keyframes** or **drop**. Do not require runtime MMD IK. Morph → future `MORP` extension (§4 / R-08) if needed.
4. **Encoding:** Rely on saba for PMX UTF-16 vs VMD Shift-JIS; do not reimplement in AY code.
5. **Quality bar for v1:** “Clip plays on the imported mesh/skeleton” — not perfect foot IK or cross-model retarget.

#### 5.7.4 Near-term vs this extension

| Need | Path |
|------|------|
| See MMD motion in engine **now** | Blender → FBX → `FBXConverter` |
| Batch / CI / no Blender | Implement §5.7 (`SabaParser` + `MMDConverter`) |
| Shared humanoid library across proportions | Still needs Retarget / bake-to-target skeleton (`AYAnimation` Phase 4) — **orthogonal** to PMX parsing |

#### 5.7.5 Acceptance sketch (R-05 / R-06 when scheduled)

- [ ] Optional saba dependency builds on Windows x64 Debug/Release
- [ ] One sample PMX → `.ayskel` + `.aymesh`; bone count / names round-trip
- [ ] Same PMX + one VMD → `.ayanm`; `AnimationPlayer` + matching skeleton produces non-identity skin matrices after tick
- [ ] No saba symbols in `interface/` or `AYAnimation` public headers
- [ ] Unit test under `unittest/` with a tiny fixture (or skipped if `AY_RESOURCE_USE_SABA=OFF`)

---

## 6. L2 — Runtime resource manager

### 6.1 Loader contract

`interface/IAYResourceLoader.h`:

```cpp
class IResourceLoader {
public:
    virtual ~IResourceLoader() = default;
    virtual bool        canLoad(const std::string& path) const = 0;
    virtual const char* getResourceType() const = 0;
    virtual std::shared_ptr<IResource>
                        load(const std::string& path) = 0;
    virtual std::shared_ptr<IResource>
                        loadAsync(const std::string& path,
                                  std::function<void(std::shared_ptr<IResource>)> cb = {}) = 0;
    virtual const char* getExtension() const = 0;       // e.g. ".aymesh"
};
```

### 6.2 LoaderRegistry and entry points

| API | Use |
|-----|-----|
| `initializeLoaders()` | Register all runtime loaders (call once at startup) |
| `ResourceRegistry::loadByPath(path)` | Debug, tests, loose files (no DB) |
| `ResourceManager::load<T>(path)` | Game / editor; DB + pak when registered; **falls back** to loose file |

`.aydep.json` is honored as a **loose-file dependency hint**: when `ResourceManager` resolves a loose `.aymesh` and the sidecar is present, dependencies (e.g. referenced `.aymat`) are loaded alongside. When absent — and when no DB is registered — the engine must still function: a missing material surfaces as a log + placeholder (`ENGINE-FOUNDATION-PLAN.md` §4.3).

### 6.3 ResourceManager, Cache, Handle, Async

| Component | File | Responsibility |
|-----------|------|---------------|
| `AYResourceManager` | `include/AYResourceManager.h` | Public load/loadAsync/unload; resolves path → loader; owns cache |
| `AYResourceCache`   | `include/AYResourceCache.h`   | Strong + weak cache; LRU; thread-safe (`std::mutex`) |
| `AYResourceHandle<T>`| `include/AYResourceHandle.h`  | Ref-counted accessor; live handles LRU-protect the asset |
| `AYAsyncLoader`     | `include/AYAsyncLoader.h`     | Real async (`std::promise`), progress callback, cancel |
| `AYHotReloadWatcher`| `include/AYHotReloadWatcher.h`| Polls `mtime`; on change, invalidates handle, reloads on next access |

Async semantics (current implementation):

```cpp
// Real async — NOT the old "fake future" bug
auto handle = manager.loadAsync<IMesh>("meshes/hero.aymesh",
    [](float p) { /* progress 0..1 */ },
    [](std::shared_ptr<IMesh>) { /* done */ });

manager.cancel(handle);                  // cancels if not yet started
manager.cancelAll();                     // cancels all pending
```

Thread safety: `ResourceCache` and `ResourceManager` use `std::mutex`. `shared_ptr` ref-count access is itself thread-safe; mutations go through the manager.

### 6.4 Path resolution

`AYAssetPath::resolveAssetPath(basePath, refPath)`:

```
refPath absolute             → use as-is
refPath relative             → directory(basePath) / refPath, normalize
refPath bare (no slashes)    → assetRoot / refPath   (if setAssetRoot() called)
```

Examples in `runtime-conventions.md` §3.

### 6.5 Hot reload

`AYHotReloadWatcher` watches a configurable set of paths; on `mtime` change, it calls `_handleHotReload`, which:

1. `unloadResource(path)` — evict L2 cache entry for the path.
2. Eager `_loadInternal(path)` (placeholder on failure) so the same tick sees fresh L2.
3. Invokes `setOnHotReload` so L3 / game can refresh (`RenderResourceManager::onResourceFileChanged`, stable handle ids).
4. Outstanding `ResourceHandle<T>::get()` re-reads the cache and picks up the new instance.

**Not** the same as `unload*` / `trimCache`: those never call `setOnHotReload`. Full rules: [`docs/ownership-contracts.md`](docs/ownership-contracts.md).

### 6.6 Error handling

Per `ENGINE-FOUNDATION-PLAN.md` §4.3:

- **Loader failure** → log + return nullptr / fallback placeholder (pink shader / missing mesh). GameLoop never crashes.
- **Converter failure** → non-zero exit in CLI; Editor shows import log panel.
- **Missing optional clip** → animation system sees `nullptr` and idles.

---

## 7. L2 — Asset interfaces

All L2 interfaces live in `interface/assetsDefs/IAY*.h` and are **the public contract**.

### 7.1 `IAYMesh`

Source: `interface/assetsDefs/IAYMesh.h`

```cpp
enum class MeshAttribute : UInt8 {
    Position    = 0,    // float[3]
    Normal      = 1,    // float[3]
    UV          = 2,    // float[2]
    Tangent     = 3,    // float[4]  (xyz=tangent, w=handedness)
    Color       = 4,    // float[4]
    SkinWeight  = 5,    // UInt8[4] bone indices + Float32[4] weights
};

struct Bounds { FVector3 center; FVector3 halfExtent; /* helpers */ };

struct VertexSkinWeight {
    UInt8   boneIndex[4];    // up to 4 bones per vertex
    Float32 boneWeight[4];   // sums to 1.0
};

class IMesh : public IResource {
public:
    // interleaved vertex buffer
    virtual UInt32         getVertexCount() const = 0;
    virtual UInt32         getVertexStride() const = 0;
    virtual const UInt8*   getVertexData() const = 0;

    // always uint32 in storage (u16 split at upload — runtime-conventions.md §5)
    virtual UInt32         getIndexCount() const = 0;
    virtual const UInt32*  getIndexData() const = 0;

    // attribute layout
    virtual UInt8          getAttributeMask() const = 0;
    struct AttributeInfo { UInt8 offset; UInt8 count; };
    virtual AttributeInfo  getAttributeInfo(MeshAttribute) const = 0;

    // submeshes
    struct Submesh { UInt32 indexOffset; UInt32 indexCount; UInt32 materialIndex; };
    virtual UInt32         getSubmeshCount() const = 0;
    virtual const Submesh* getSubmeshes() const = 0;

    // material slot paths (virtual paths, resolved by AYAssetPath)
    virtual UInt32         getMaterialSlotCount() const = 0;
    virtual const char*    getMaterialSlot(UInt32) const = 0;

    // bounds
    virtual Bounds         getBounds() const = 0;
    virtual Bool           hasBounds() const = 0;

    // skin weights — Phase 0 critical path (RD-02)
    virtual Bool                        hasSkinWeights() const = 0;
    virtual const VertexSkinWeight*     getSkinWeights() const = 0;

    // LOD stub
    virtual UInt32   getLODCount() const = 0;
    virtual IMesh*   getLOD(UInt32) = 0;

    // Extension chunks (use before adding a new file type)
    struct Extension { UInt32 type; UInt32 size; const UInt8* data; };
    virtual UInt32               getExtensionCount() const = 0;
    virtual const Extension*     getExtension(UInt32) const = 0;
    virtual const Extension*     findExtension(UInt32 fourcc) const = 0;

    // helpers
    inline Bool hasAttribute(MeshAttribute a) const;
    inline const FVector3* getPositions() const;
    inline const FVector3* getNormals()   const;
    inline const FVector2* getUVs()       const;
    inline const FVector4* getTangents()  const;
    inline const FVector4* getColors()    const;

    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC   = 0x484D5941;   // 'AYMH'
};
```

### 7.2 `IAYSkeleton`

Source: `interface/assetsDefs/IAYSkeleton.h`

```cpp
struct Bone {
    std::string name;
    int         parentIndex;             // -1 == root
    Float4x4    inverseBindMatrix;
    FVector3    localPosition;
    FQuaternion localRotation;
    FVector3    localScale;
};

class ISkeleton : public IResource {
public:
    virtual size_t        getBoneCount() const = 0;
    virtual const Bone*   getBones() const = 0;
    virtual int           findBone(const char* name) const = 0;
    virtual int           getParentBoneIndex(size_t boneIndex) const = 0;

    virtual const Float4x4*    getInverseBindMatrices() const = 0;
    virtual const FVector3*    getLocalPositions()      const = 0;
    virtual const FQuaternion* getLocalRotations()      const = 0;
    virtual const FVector3*    getLocalScales()         const = 0;

    virtual size_t    getRootBoneCount()  const = 0;
    virtual const int* getRootBoneIndices() const = 0;

    static constexpr const char* TYPE = "Skeleton";
    static constexpr UInt32 VERSION = 1;
};
```

`IAnimation` references skeleton **by bone name** (see §7.3). Bone-index resolution happens in `AYAnimation` at playback time. This decouples `.ayanm` from skeleton GUIDs.

### 7.3 `IAYAnimation`

Source: `interface/assetsDefs/IAYAnimation.h`

```cpp
enum class AnimTrackType : UInt8 { Vector3, Quaternion, Float };

struct AnimTrack {
    std::string  nodeName;          // == skeleton bone name (or node name)
    std::string  property;          // "position" | "rotation" | "scale"
    AnimTrackType valueType;
    std::vector<Float32> times;     // seconds
    std::vector<Float32> values;    // interpret per valueType
};

class IAnimation : public IResource {
public:
    virtual const char* getName() const = 0;
    virtual Float32     getDuration() const = 0;
    virtual Float32     getTicksPerSecond() const = 0;

    virtual UInt32      getTrackCount() const = 0;
    virtual const char* getTrackNodeName(UInt32) const = 0;
    virtual const char* getTrackProperty(UInt32) const = 0;
    virtual AnimTrackType getTrackType(UInt32) const = 0;

    virtual UInt32      getTrackKeyframeCount(UInt32) const = 0;
    virtual const Float32* getTrackTimes(UInt32) const = 0;
    virtual const FVector3*    getTrackVector3Values(UInt32) const = 0;
    virtual const FQuaternion* getTrackQuaternionValues(UInt32) const = 0;
    virtual const Float32*     getTrackFloatValues(UInt32) const = 0;

    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC   = 0x4E4D5941;   // 'AYNM'
};
```

### 7.4 `IAYMaterial`

Source: `interface/assetsDefs/IAYMaterial.h`

```cpp
class IMaterial : public IResource {
public:
    virtual const char* getName()  const = 0;
    virtual const char* getShader() const = 0;       // Phoskia virtual path

    // typed accessors
    virtual Float32  getFloat (const char* name) const = 0;
    virtual Int32    getInt   (const char* name) const = 0;
    virtual Bool     getBool  (const char* name) const = 0;
    virtual FVector2 getVector2(const char* name) const = 0;
    virtual FVector3 getVector3(const char* name) const = 0;
    virtual FVector4 getVector4(const char* name) const = 0;
    virtual FVector4 getColor  (const char* name) const = 0;
    virtual const Float32* getMatrix(const char* name) const = 0;

    virtual const char* getTexture(const char* name) const = 0;   // virtual path

    static constexpr UInt32 VERSION = 1;
    static constexpr UInt32 MAGIC   = 0x544D5941;   // 'AYMT'
};
```

`shader` is a **Phoskia source path** (e.g. `shaders/pbr.phoskia`), resolved by the bridge in `AYRenderer` via `runtime-conventions.md` §1. `.ayshader` legacy blobs are **not** loaded at runtime.

### 7.5 `IAYTexture`, `IAYAudio`, `IAYVideo`, `IAYFontAsset`, `IAYPhysics`, `IAYScript`, `IAYShader`

Each is a thin, stable interface — see headers in `interface/assetsDefs/`. Two cross-cutting notes:

- `IAYAudio::Format` selection policy is documented inline (extension > size > manual override).
- `IAYShader` is **legacy**; runtime uses `AYShader::ShaderResourcePool` directly with Phoskia source.

---

## 8. Public API surface (CR-enforced)

From `runtime-conventions.md` §8 + `ENGINE-FOUNDATION-PLAN.md` §4.1.

### 8.1 PUBLIC — propagated to dependents

```
AYResource.h
interface/**
interface/assetsDefs/**
include/AYResource*.h
include/AYAssetPath.h
include/AYAsyncLoader.h
include/AYHotReloadWatcher.h
include/AYLooseDependency.h
```

Game / ECS / editor code includes `interface/assetsDefs/IAY*.h` only. It links `AYResource` and calls `AYResourceManager::load<T>` / `loadAsync<T>` / `AYResourceHandle<T>`.

### 8.2 PRIVATE — this module + tests only

```
include/assetsImpl/**   — concrete IResource classes (AYMesh, AYMaterial, …)
include/Loader/**       — per-format loaders
include/Converter/**    — offline converters
```

A PR that exposes `assetsImpl/AYMesh.h` to another module **fails review**.

### 8.3 Cross-module include / link rules

(From `ENGINE-FOUNDATION-PLAN.md` §4.1.)

| Module | May include from AYResource | Must NOT include |
|--------|----------------------------|------------------|
| `AYEntity` (ECS) | `interface/assetsDefs/IAY*.h` | `include/assetsImpl/*`, `include/Loader/*`, `include/Converter/*` |
| `AYRenderer` | `IAY*` (+ `assetsImpl` only inside `src/detail/` if cast needed) | n/a (owns L3 bridge) |
| `AYAnimation` | `interface/assetsDefs/IAYMesh.h`, `IAYSkeleton.h`, `IAYAnimation.h` | `assetsImpl/*`, anything bgfx |
| `AYEditor` | `interface/**`, `IAYConverter.h` / `AYImportJob.h`, `AYResourceBootstrap.h` | `include/Loader/*`, `assetsImpl/*` |

Known transitional exceptions: [`docs/private-include-allowlist.txt`](docs/private-include-allowlist.txt).

---

## 9. L1 ↔ L3 bridge (the part renderer cares about)

This is the contract that lets `AYRenderer` upload without re-parsing.

| L1 layout | L2 query | L3 upload (bgfx) |
|-----------|----------|------------------|
| `Position` attr | `IMesh::getAttributeInfo(Position)` | `VertexAttribute::Position`, 3×f32 |
| `Normal`   attr | `IMesh::getAttributeInfo(Normal)`   | `VertexAttribute::Normal`,   3×f32 |
| `UV`       attr | `IMesh::getAttributeInfo(UV)`       | `VertexAttribute::TexCoord0`,2×f32 |
| `Color`    attr | `IMesh::getAttributeInfo(Color)`    | (preset TBD; Phase 0–1) |
| `Tangent`  attr | `IMesh::getAttributeInfo(Tangent)`  | (preset TBD; Phase 0–1) |
| `SkinWeight` attr | `IMesh::hasSkinWeights()` / `getSkinWeights()` | Bone matrix SSBO + skinning shader (Phase 1, `RD-03..RD-05`) |
| `Extension[MORP]` | `IMesh::findExtension('MORP')` | Morph blend (Phase 3, `RD-06`) |
| Indices | `IMesh::getIndexData()` (u32) | `createMesh` if max<65536 else `createMesh32` + `BGFX_BUFFER_INDEX32` |

Full spec with shader-uniform names and parameter conventions: `docs/runtime-conventions.md` §1–§7.

---

## 10. Phase 0 backlog (what AYResource owns in `ENGINE-FOUNDATION-PLAN.md` §6 Phase 0)

| ID | Deliverable | Acceptance | Status |
|----|-------------|------------|--------|
| **R-01** | Document & enforce L1/L2 API surface; add `IAYMesh` extension chunks | CR checklist in `runtime-conventions.md` §8 + this design §8 | **In progress** (this doc) |
| **R-07** | `ResourceManager` async load + progress callback | Editor can poll load state | ✅ Done — `AYAsyncLoader` exposes `ProgressCallback` + `cancel` |
| **R-08** | Morph target storage (extension chunk `MORP` or `.aymorph`) | Round-trip morph count + delta size | 🔄 Deferred — extension chunk path agreed; need unit test once `RD-06` lands |

**Phase 0 exit gate** (`RD-02` in `AYRenderer`): static FBX character loads; skin weights survive GPU upload in **bind pose**. AYResource's contribution to this gate is keeping `IMesh::hasSkinWeights()` / `getSkinWeights()` stable and loadable from the existing `.aymesh` files. **Do not break the `SkinWeight` attribute bit while Phase 0 is open.**

---

## 11. Tests (current)

```
unittest/
├── AYTest_AssetPath.cpp              — path resolution
├── AYTest_AnimationLoader.cpp        — .ayanm parse
├── AYTest_AudioLoader.cpp            — .ayaudio parse + format policy
├── AYTest_FBXConverter.cpp           — FBX → IntermediateAsset round-trip
├── AYTest_FontLoader.cpp
├── AYTest_MaterialFile.cpp           — multi-material bundle
├── AYTest_MeshLoader.cpp             — .aymesh parse (incl. skin weights)
├── AYTest_PublicApiSurface.cpp       — guards that PRIVATE headers aren't leaked
├── AYTest_ResourceBootstrap.cpp
├── AYTest_ResourceCore.cpp           — cache + handle + LRU
├── AYTest_ResourcePhase2.cpp         — async + progress + cancel
├── AYTest_SkeletonLoader.cpp
├── AYTest_TextureConverter.cpp       — preprocess / copy / parallel
├── AYTest_VideoLoader.cpp
├── AYTest_materialConverter.cpp      — .aymat → Phoskia ref
└── main.cpp
```

`AYTest_PublicApiSurface.cpp` is the contract guard: (1) compiles the public umbrella without private includes; (2) scans `AYRuntime` production sources for `assetsImpl` / `Loader` / `Converter` includes and fails on new hits outside [`docs/private-include-allowlist.txt`](docs/private-include-allowlist.txt). Pipeline ownership tests: `AYTest_ResourcePipelineP6.cpp`.

---

## 12. What is NOT in AYResource (deferred to other modules / phases)

| Item | Owner | When |
|------|-------|------|
| `RenderMesh` (L3) | `AYRenderer` | now |
| Bone matrices, skinning, `AnimationPlayer` | `AYAnimation` | Phase 1 (AN-01..AN-03) |
| State machine, IK, motion matching, retarget | `AYAnimation` | Phase 2+ (`ENGINE-FOUNDATION-PLAN.md` §6 Phase 4) |
| glTF 2.0 real converter | `AYResource` | Phase 1 R-04 |
| PMX / VMD converters (`MMDConverter` + saba) | `AYResource` | Deferred — **spec in §5.7**; schedule with R-05/R-06 when batch MMD is needed |
| Pak/DB format, streaming | (future `AYStorage`) | Phase 4 |
| Scene / prefab format | `AYSerializer` (future) | now (JSON stub in old design §13) |

---

## 13. Revision history

| Date | Author | Change |
|------|--------|--------|
| 2026-06-04 | Content team | Initial draft |
| 2026-06-09 | Content team | Added GUID system, SQLite schema drafts, precision compression framework, pak design |
| **2026-07-06** | **Content team** | **v2.0 — major realignment with `ENGINE-FOUNDATION-PLAN.md` v1.0 and `runtime-conventions.md`.** Trimmed speculative / out-of-scope sections (precision compression, full SQLite schema, pak design). Added: explicit non-goals, three-layer module layout, current vs target state, IntermediateAsset detail, L1↔L3 bridge map, CR-enforced public/private API surface, Phase 0 backlog with R-01/R-07/R-08 status, deferred items mapped to owning modules. Preserved all concrete API signatures from the actual headers — no API removed. |
| **2026-07-27** | Content / agent | **§5.7 MMD (PMX/VMD) via saba** — future optional Parser frontend reusing IntermediateAsset + existing typed Converters; CMake `AY_RESOURCE_USE_SABA`; near-term path remains Blender→FBX. Updated §5.6 coverage table + §12 deferred row. |
| **2026-08-02** | Content / agent | **P6 ownership** — §6.5 hot-reload matches eager reload + `setOnHotReload`; §8/§11 PublicApiSurface scan + allowlist; link `ownership-contracts.md`. |