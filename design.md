# AYResource — Design (v2.1)

## 对话作者文档的发布门禁（2026-10-27）

ProjectBuild 作者目标可选链接 AYProgressionJson；普通 AYResource runtime 不增加
对话依赖。选中的 `.dialogue.json` 必须为 Raw，最大 1 MiB，规划和执行均用同一
版本化解码器检查图结构及跨文档 ID 唯一性。Raw 项的 `cacheKey` 存规划时
`dialogue-source-v1` SHA-256；执行期先复查源，发布前复制到私有 staging 并按同一
身份核对，随后只发布冻结副本。失败不替换旧输出。无 AYProgressionJson 的作者
构建会拒绝选中的对话文档，避免把未验证 JSON 当成可发布对话。此阶段不定义
对话 Cook 格式、运行时 Loader 或自动发现跨语言及媒体依赖。

可选 `content.dialogueBindings` 是 Assets 相对的 Raw JSON，schema 1 含
`documents`（1..1024）、`locales`（1..64）、`voice` 和 `sequence`
（各至多 4096 键）。规划和执行均从选中资产表解析这些路径，不接受逃逸、
排除、Cook 或符号链接。所有选中对话文档都必须列入。每份 locale JSON 上限
1 MiB，需有非重复的归一化语言标签，并覆盖全部被引用的非空文本键；
Voice 绑定须是可解码的 `.aud`（至多 128 MiB），Sequence 绑定须是 `.sqc`
且由现有 Sequence 门禁复核。映射及引用的 Raw 文件保存 SHA-256 规划身份，
执行时复核并逐项复制到私有 staging；发布只读取冻结副本。未声明映射时仍只
做单文档结构与 ID 检查。它不把 `.seq` 或音频作者文件自动转换为对话绑定，
也不提供运行时键解析策略。
发布 manifest 和资源数据库从映射文件记录指向所有已核对文件的依赖边，
便于审计包内闭包；ProjectBuild 与运行时可共用 AYProgression 的纯值
`decodeBindingsJson`，但游戏仍需安装文件读取与内容键解析器。
语言目录可以直接通过 `Localization::loadFromJson` 从 Pak 字节安装；包内
读取回归同时验证 `PresentationController` 的首次展示与恢复展示规则。

## Sequence 作者资源发布边界（2026-10-26）

可选 AYSequenceAssetIO/AYSequenceCookIntegration 只链接作者工具目标，
Resource runtime 只登记 `.sqc` 类型元数据。`.seq` 的转换器返回一件
`SceneSequence` cooked 资源和每个引用的独立依赖边；不把场景、动画或音频
塞进包封。ProjectBuild 的 sourceClosure 让引用文件内容影响缓存身份，
规划与执行两次核对可发布路径，缓存命中仍需比对转换契约、图和实际包封
签名。CookShip 在替换目录或 Pak 前重新验证引用闭包。

## 2026-10-22 — 分层刷新确认

轻量`ResourceRefresh.h`定义消费者状态及拥有字符串/值的报告，不依赖Renderer或bgfx。
原两参数bool方法代理到同一内部CPU批次实现；报告入口在CPU失败时返回错误且不含消费者结果，
成功则逐规范路径同步通知。CPU事务不因为消费者`Retained`或`Failed`回滚。

legacy与reporting回调共享一个slot；reporting注册使用单调token，清理仅匹配当前token。
token耗尽显式抛错，不回绕复用。legacy替换清零当前所有权但不重置序列。
通知前复制functor与消费者名，回调内替换不会销毁执行中的对象，下一路径采用新slot。
这不是并发析构保护，所有操作仍要求同一owner线程。

消费者异常转为`Failed`，空结果为`Unconfirmed`；legacy正常返回也只能标为`Unconfirmed`。
最多保留64项结果，超量时末项为明确`Failed`；消费者/变体标签最多128字节，诊断最多1024字节。
这些是报告保留预算，不是限制第三方callback内部的分配，也不是设备OOM恢复保证。
报告的opaque id/context不构成资源租约或跨上下文有效性证明。

本阶段完整Resource384项中372通过、12个可选FBX夹具跳过，4631断言无失败；
六个相关ASan套件64例486断言无失败/跳过，配置关闭Renderer/Shader作者模块。
报告在根`out/render-sampling-20261021/refresh-{resource-final,asan-final-*}`。
ASan首次因默认PATH缺运行库未启动，另一次cmd路径拼接也未执行测试；
最终在同一VS环境完成，启动失败与通过结果分别保留，不当作生产缺陷或累计通过。

## 2026-10-22 — Texture v2 与采样作者配方

`ITexture::VERSION=2`；writer 保持历史 magic `0x48545841` 和36字节头布局，插入16字节 packed sampler：
min/mag/wrapU/wrapV/wrapW/sRGB各1字节、reserved UInt16=0、anisotropy Float32、lodBias Float32。
均沿用当前 little-endian 资产约定；imageDataSize只计编码图像，偏移为52。v1图像偏移36，sampler取历史默认。
新运行时可读旧资源，旧严格版本读者不能读 v2；不批量改写既有文件，显式保存/重导入才写新版本。

读取先复制 packed 头并验证 magic/version、正尺寸、合法完整或前缀层级、格式、每层准确字节数和 EOF，
再验证 sampler 有效枚举、sRGB 0/1、reserved0、anisotropy有限1..16、lodBias有限-16..16。
图像总数据≤256 MiB。所有临时解析成功后才替换对象字段；失败不破坏原像素、sampler和 loaded 状态。
保存核对完整且连续的 mip 表；失败保留 outData。computeMipSize 在乘法前检查 UInt32 上限，未知格式返回0。
公开 mip buffer 读取核对偏移和长度，测试创建器拒绝溢出尺寸/零棋盘大小。
Loose 图片先读取有界文件并检查维度，再进行像素解码；与 cooked 容器共享256 MiB字节预算。

ImportOptions 追加采样覆盖和层级保留字段；不扩展 IConverter vtable。
ImportJob 在 converter 完成后、最终指纹和事务发布前，对 typed Texture 应用设置并重新编码；
raw 图片明确拒绝有覆盖的请求。来源 sRGB 被保留，不能用全模型覆盖把法线/标量贴图强制变成颜色贴图。
keepTextureMipmaps=false 只保留已有基础层；true不补齐 DDS。resource.size随重新编码更新，原像素 GUID 策略保留。
作者 recipe 保持version1，textureSampling为可选字段；默认未覆盖设置不写字段以兼容旧缓存语义。
非默认设置参与指纹、恢复、批量传播与离线计划。GPU设备能力、所有权和回退由 Renderer 负责。

## 2026-10-21 — 来源配方、事务发布和显式驻留资源刷新

ImportJob 在相邻的独占临时目录调用 converter，校验非空 typed 输出及发布路径，备份旧文件后逐个替换。
失败逆序恢复；恢复失败保留事务目录以便人工恢复。元数据最后发布。
发布/回滚的 rename 和回滚 remove 共享 AYIO implementation-only PublicationRetry 策略。
Windows native 5/32/33 冲突限26次/250ms；其他错误及非 Windows 路径不额外重试。
真实 Session 重复回归抓到扫描读者持有 .dep 导致重命名失败；原生测试在首次真实失败后通过
内部非抛出 notice 释放读者，验证恢复、metadata 最后发布与无关文件保留，持续锁定仍逆序回滚。
主源与 converter 真正打开的依赖在读取前采集 SHA-256/大小，转换后再次核对。
缓存以内容为准；只改时间戳不重建，保留时间戳修改字节会重建。
缓存 `.dep` 的 import/version2 记录来源、完整设置、输入/输出指纹；逐资源 `.dep` 仅保存该资源的出边。
GLTFConverter 复用 FBXConverter 的场景管线，移除生成占位八顶点网格的旧实现。

sourceId 是作者配方身份，outputNamespace 是受限单路径组件。项目内新来源自动分配独立身份，
已有可恢复的同源旧导入继续使用原路径。迁移后来源路径可变，已绑定身份与命名空间不变。
资源叶名仍由既有模型/子资源命名决定；不承诺 DCC 重命名子对象后自动修复所有引用。
作者配方 `ay.import-recipe` v1 与可删除缓存分开，源/输出/root 可为项目相对路径。
Resource 提供有界 UTF-8 codec/读取器，Editor 负责保存和 UI；ProjectBuild 消费相同配方，
不允许 Resource 反向依赖 Editor。离线构建已通过26个 ProjectBuildTests（包含模型设置、真实贴图依赖与配方改动）验证。

ResourceCache::replace 同时更新 strong/weak/grace，避免 put 的保留旧值语义让 GPU 看到旧对象。
refreshPublishedResources 先完整解码驻留集合，失败不发布；成功替换后才通知现有 reload callback。
不提供跨线程多项缓存的原子读快照，不撤回外部已经借出的旧对象。
显式发布刷新将报告路径与 weakly_canonical 物理路径组成去重集合，再从已知类型表快照中
补入物理路径相同的已知绝对路径键，兼容配方物理路径与 Renderer 目录联接路径双向差异。
L2 已淘汰的别名仍通知 L3，不在发现阶段加载它；observer 自行按需加载，显式 unload 会忘记该键。
类型表锁不跨文件系统检查、解码或回调。按键排序使别名
通知顺序可重复，全部解码成功后才统一替换并通知。缓存身份/包查找规则不变，不搜索未知别名，
不承诺硬链接或全部 Unicode 大小写等价。原生目录联接回归只报告物理路径并重复该输入，验证
两份旧借用、两种驻留拼写、标签保留、去重通知与通知前全部替换、坏负载不改变已有条目。

### 主源重新定位

ImportOptions.outputBaseName 是可选的单个便携 UTF-8 文件 stem（最长160bytes）；
默认空保持旧命名。FBX/glTF/Texture 三条作者管线支持覆盖，不支持的 converter 拒绝非空值。
它进入完整 settings/cache identity；缺省旧 metadata 可读取，但完整设置比较会要求重新构建。
模型 typed writer/材质槽/骨架动画依赖统一用此 stem，独立图像也保留原 texture/metadata 路径。
作者 sourceId 显式匹配才允许发布 metadata 的来源路径变化。没有新增运行时格式。

### 真实来源闭包与贴图绑定补充

ProjectBuild 在规划阶段通过现有场景 parser 发现 main/buffer/image 输入并采集快照，
复查稳定后记录 importDependencies；后续计算 key 不重新解析模型，不散列无关旁文件。
新鲜计划仍会发现新增依赖。执行前重核已知输入和当前配方集合；增删/改作者设置要求重新规划。

glTF 的 G/B 标量分离、AO 与压缩 embedded PNG/JPEG 使用原有 .txr 编解码。
Assimp 的 mHeight=0 表示压缩字节流长度，而不是零高 RGBA 图。内嵌图片按语义绑定发布，
不再留下未解析的 *N 引用。单像素轴的 mip 使用真实源宽高；编码器输出大小必须匹配目标格式。
父目录纹理路径不会进入输出；不安全来源名及碰撞按来源相对键生成确定性后缀。
命名哈希仅用于消歧，不是内容 GUID 或认证机制。重导入保留既有安全名称。

### 坐标、完整输出与 DDS 边界

glTF Auto 不再走忽略单位覆盖的旧 Assimp 左手分支；以规范 RH/Y-up/+Z-forward/米制构造
同一个坐标 wrapper，显式单位覆盖保留，未启用的 Manual 轴字段不参与 Auto。导入契约 v24
使旧派生缓存重建。真实 Assimp 导出的单骨 FBX 测试检查 Mesh skin、Skeleton 和 Animation
并核对真实转换路径间的依赖，避免根据猜测的骨架/clip 名重建不存在的边。

每类 typed writer 的产物数量必须匹配 IntermediateAsset；场景依赖不能指向未发布的资源。
原始字节复制只是底层操作，完成的 ImportJob 会解码检查图片/typed texture 后才发布。
批量 Texture 转换的 1 像素轴和奇数尺寸 mip 以当前层实际宽高下采样，禁止以目标格式相同为由
跳过发生内容变化的源。缓存 metadata v2 使之前可能发布无效 DDS/raw-image 的缓存失效；
作者配方仍 v1，旧 metadata v1 可恢复作者设置但不能直接命中。

DDS_HEADER/DX10 只接受单张 2D、UNORM BC1/3/4/5/7，最多16层且不得超过完整 mip 链；
精确校验块长与 EOF 后，用已有 Texture writer 封装 .txr，不把原 DDS 改后缀冒充 typed 文件。
拒绝数组/cube/volume/signed/未知格式；不新增运行时格式或 GPU 色彩空间机制。
参考：[Microsoft DDS_HEADER](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header)、
[DDS_HEADER_DXT10](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header-dxt10)、
[Khronos glTF specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)。

## 2026-10-20 — 作者材质读取的原子性

单 Material 的 v1/v2/v3 解码使用有界 memcpy，从任意对齐的完整 bytes 构造临时对象，
校验精确负载/count/tag、非空唯一参数名和 bool，再发布字段。
C-string 身份字段 name/shader/参数名不允许 NUL；未知 String 参数仍保留一般字节内容。
Float4x4 按 16 个 float 读取，修复旧实现只读4个数导致后续参数错位的问题。
编码前检查 255 参数上限、字符串/负载的 UInt32 可表示性，失败保留输出缓冲。
没有新文件或虚接口版本，bundle codec 与资源/GPU ownership 不变。
MaterialFileTests 覆盖旧版本、矩阵/未知参数往返、每个前缀截断、非法 tag/count/name、
极端长度、非对齐输入和失败不变；同组在独立 MSVC ASan 构建执行。

## 2026-10-05 — ProjectBuild 默认 Phoskia 阶段

验证：ProjectBuild默认无宏程序及纯helper闭包发布成功，破坏模块后不替换既有发布；
Pak选择规划失败。完整Resource快组169用例/965断言、integration128选择/1714断言通过，
后者12项外部FBXfixture未提供/未opt-in而跳过。深cwd写包失败修复为native长路径读写，
AYShader额外覆盖最终包目录超过260字符的往返；不通过改短测试目录隐藏真实问题。

staging Content 完成后、package manifest 与目录发布前执行 ProjectShaderProduction：
校验代码产物相邻 Shaders 的每个 engine 包并重写受限包；对选中 raw/loose 源调用共享
collectShaderBuildJobs，要求模块也在选中集合，默认无宏程序离线编译到相同目标的 project 包。
结果 shaderPackages 进入原 manifest，production.policy 为运行时严格策略标记。
编译/校验失败不触及既有发布目录；不用 ResourceManager 的旧 .shb loader，不新增资产格式。
作者目标私有依赖 AYShader，运行时 Resource 不链接项目编译阶段；不支持跨目标 host 构建，
所选 profile 的平台必须匹配配置。大小写扩展名按 AYAssetFormat 识别，不另维护后缀表。

**Version:** 2.1.0
**Date:** 2026-09-18

> **2026-09-01 — `.ayatlas` v1**: `.ayatlas.json` is the author format and
> `AtlasConverter` emits `.ayatlas` v1 (`AYAT`). `IAtlas` exposes the referenced
> texture path, atlas/tile dimensions, dense-grid counts, gutter, filter, and
> per-axis wrap. The asset owns no GPU handle; AYRenderer owns L3 texture
> residency.

**Status:** Living document, aligned with [`ENGINE-FOUNDATION-PLAN.md`](../../../../ENGINE-FOUNDATION-PLAN.md) and [`docs/runtime-conventions.md`](docs/runtime-conventions.md)
**Last revised:** 2026-09-21（骨骼引用安全烘焙与发布闭包门禁已实现）
**Owner:** Content engineer

---

## 0. Reading guide

角色资产的尺寸、绑定、版本、动画兼容与多纹理验收统一见 [AYHumanoid 标准骨架与参考模型规范](../../AYDocs/AYHUMANOID-STANDARD.md)；FBX 操作见 [外部角色与动画导入标准](docs/external-character-animation-import-standard.md)。标准参考资产及验收工具待实施，设计目标不表示当前 importer 已实现相应检查。

原始骨架保留、可绑定骨架的映射/重定向配置资源、编辑器状态与发布清理统一见 [骨骼动画资源管线设计](../../AYDocs/SKELETAL-ANIMATION-RESOURCE-PIPELINE.md)。SKA-01～16 的优先级、owner、实现状态和验收仅在该文档 §7 维护；其中当前正式资源集的 SKA-07/08 已完成，完整源骨架导入 SKA-03 仍待实施。

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

**EventBus:** `AsyncLoader` posts `ResourceLoadCompleteEvent` / `ResourceLoadFailedEvent` when an async load finishes (see `AYRuntime/AYEventSystem/README.md`). Sync `_loadInternal` paths do not post.

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
7. **Public API surface is narrow and stable.** Game / ECS / editor code includes `interface/AYResource/assetsDefs/I*.h` only; concrete impls are private to this module.

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
│   ├── AYResource/IResourceLoader.h      # Loader contract
│   ├── AYResource/IConverter.h           # Converter contract
│   └── assetsDefs/              # Per-asset L2 interfaces (PUBLIC)
│       ├── AYResource/assetsDefs/IMesh.h
│       ├── AYResource/assetsDefs/ISkeleton.h
│       ├── AYResource/assetsDefs/IAnimation.h
│       ├── AYResource/assetsDefs/IMaterial.h
│       ├── AYResource/assetsDefs/ITexture.h
│       ├── AYResource/assetsDefs/IAudio.h
│       ├── AYResource/assetsDefs/IVideo.h
│       ├── AYResource/assetsDefs/IFontAsset.h
│       ├── AYResource/assetsImpl/Shader.h          # legacy; offline tooling only
│       ├── AYResource/assetsDefs/IPhysics.h
│       └── AYResource/assetsImpl/Script.h
│
├── include/                     # PUBLIC headers + PRIVATE impl
│   ├── AYResource/ResourceManager.h      # PUBLIC: cache + async load
│   ├── AYResource/ResourceCache.h        # PUBLIC: weak/strong cache
│   ├── AYResource/ResourceHandle.h       # PUBLIC: ref-counted handle
│   ├── AYResource/ResourceRegistry.h     # PUBLIC: loader routing
│   ├── AYResource/ResourceBootstrap.h    # PUBLIC: initializeLoaders()
│   ├── AYResource/AsyncLoader.h          # PUBLIC: real async + progress + cancel
│   ├── AYResource/AssetPath.h            # PUBLIC: path resolver (dev/asset root)
│   ├── AYResource/HotReloadWatcher.h     # PUBLIC: file-watcher for hot-reload
│   ├── AYResource/LooseDependency.h      # PUBLIC: .aydep.json loose-side loader
│   ├── AYResource/IntermediateAsset.h    # PUBLIC-ish: shared between Conv + tests
│   ├── assetsImpl/              # PRIVATE: concrete IResource classes
│   │   ├── AYResource/assetsImpl/Mesh.h / .cpp
│   │   ├── AYResource/assetsImpl/Skeleton.h / .cpp
│   │   ├── AYResource/assetsImpl/Animation.h / .cpp
│   │   ├── AYResource/assetsImpl/Material.h / .cpp
│   │   ├── AYResource/assetsImpl/Texture.h / .cpp
│   │   ├── AYResource/assetsImpl/Audio.h / .cpp
│   │   ├── AYResource/assetsImpl/Video.h / .cpp
│   │   ├── AYResource/assetsImpl/FontAsset.h / .cpp
│   │   ├── AYResource/assetsImpl/Shader.h / .cpp
│   │   ├── AYResource/assetsImpl/Physics.h / .cpp
│   │   └── AYResource/assetsImpl/Script.h / .cpp
│   ├── Loader/                  # PRIVATE: per-format loaders
│   │   ├── AYResource/Loader/MeshLoader.h / .cpp
│   │   ├── AYResource/Loader/SkeletonLoader.h / .cpp
│   │   ├── AYResource/Loader/AnimationLoader.h / .cpp
│   │   ├── AYResource/Loader/MaterialLoader.h / .cpp
│   │   ├── AYResource/Loader/TextureLoader.h / .cpp
│   │   ├── AYResource/Loader/AudioLoader.h / .cpp
│   │   ├── AYResource/Loader/VideoLoader.h / .cpp
│   │   ├── AYResource/Loader/FontLoader.h / .cpp
│   │   ├── AYResource/Loader/ShaderLoader.h / .cpp       # legacy
│   │   ├── AYResource/Loader/ScriptLoader.h / .cpp
│   │   ├── AYResource/Loader/PhysicsLoader.h / .cpp
│   │   └── AYResource/Loader/MaterialFile.h / .cpp        # multi-material bundle helper
│   └── Converter/               # PRIVATE: offline converters
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/FBXConverter.h / .cpp
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/FBXParser.h / .cpp
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/GLTFConverter.h / .cpp       # stub today
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/MeshConverter.h / .cpp
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/SkeletonConverter.h / .cpp
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/AnimationConverter.h / .cpp
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/MaterialConverter.h / .cpp
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/TextureConverter.h / .cpp
│       ├── AYResource/Converter/ITextureCompressor.h         # BC1/BC3/BC5/BC7 abstraction
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/TextureCompressor.h / .cpp   # factory
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/AudioConverter.h / .cpp
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/VideoConverter.h / .cpp
│       ├── AYResource/Converter/AYResource/Converter/AYResource/Converter/FontConverter.h / .cpp
│       └── AYResource/Converter/AYResource/Converter/AYResource/Converter/ShaderConverter.h / .cpp     # legacy
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
│   ├── AssetsImpl/                      # mirrors include/AYResource/assetsImpl/
│   ├── Loader/                          # mirrors include/AYResource/Loader/
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
| `.aymesh` | `'AYMH'` | v1 | Static mesh + optional local skin weights (`SkinWeight` bit `1<<5`) + `JPAL` per-submesh palette + `Extension[]` | Skin cook compacts global skeleton indices into renderer-profiled draw palettes; `MORP` remains an extension chunk |
| `.ayskel` | `'AYSK'` | v1 | Bone hierarchy + bind pose + inverse bind matrix | Stable; bone-name hash optional |
| `.ayanm` | `'AYNM'` | v1 | Node-name-keyed tracks (pos/rot/scale) | Stable; ensure skeleton-binding metadata (skel path + bone-name table) |
| `.aymat` | `'AYMT'` | v1 | Phoskia shader path + typed params + texture slots | Stable; shader variant tags (`skinned`, `morph`) as params, not new file type |
| `.aytex` | — | v1 | Pixel data + mipmaps | Stable |
| `.ayfont` | — | v1 | Glyph atlas | Stable |
| `.ayaudio` | — | v1 | **Ship L1 = PCM** (see §4.2.1) | Cook from WAV/MP3/OGG; Dev may load loose sources without cook |
| `.ayvideo` | — | v1 | RGBA frames | Stable (decode stack incomplete; not a player) |
| `.ayscript` | — | v1 | Source text | Stable |
| `.ayphys` | — | v1 | Shape parameters | Stable |
| `.ayshader` | — | v1 | **Legacy** GLSL/HLSL blob | **Deprecated for runtime.** Phoskia is the new shader source format. See `runtime-conventions.md` §1. |

### 4.2.1 Audio authoring vs cook (LOCKED 2026-08-13)

Aligned with `AYAudio/design.md` §2.3:

| Mode | Source on disk | Runtime path |
|------|----------------|--------------|
| **Dev / Editor** | `.wav` / `.mp3` / `.ogg` (common formats) | `AudioDecoder` → PCM `IAudio` → `AYAudio::registerClip` / `playStream` |
| **Ship / Optimize** | cooked `.ayaudio` (PCM L1) | `AudioLoader` → PCM `IAudio` → same AYAudio APIs |

**Rules:**
1. Decode belongs in **AYResource** (`AudioDecoder` + `AudioConverter`), never in the AYAudio mixer.
2. Cook is **optional until pack or explicit optimize** — not required for every save in the Editor.
3. Release builds must not depend on loose MP3/OGG (gate with `AY_AUDIO_LOOSE_FORMATS` or equivalent).
4. v1 `.ayaudio` payload = **PCM** (S16 or F32 interleaved + header). Compressed-inside-L1 (OPUS) is a later extension chunk, not a blocker.
5. Implementation order: see `AYAudio/design.md` §2.3.1 slices **A1–A6**.

**Gap today:** none for v1 decode/cook path — `AudioDecoder` + loose loaders + cook shipped 2026-08-13. Optional follow-ups: golden MP3/OGG fixtures, resample on `setOutput*`, compressed payload inside `.ayaudio`.

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
    uint32_t type        ('JPAL' | 'MORP' | 'CLTH' | 'PHYS' | 'SKEL' | user-defined)
    uint32_t size
    uint8_t  data[size]
```

`JPAL` stores one local-to-global joint range per submesh. Cooked vertex `boneIndex`
values address that submesh's range, while each palette entry addresses the complete
`.ayskel`. The current uniform-buffer profile limits a draw palette to 128 joints;
the skeleton itself is not capped. If a source submesh exceeds the profile, the
converter partitions it on triangle boundaries and preserves material assignment,
vertex attributes, and morph deltas.

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

`IntermediateAsset` (in `include/AYResource/IntermediateAsset.h`) is the format-neutral in-memory representation:

```cpp
struct MeshData       { name, positions, normals, uvs, tangents, colors,
                        indices, submeshes, materialSlots, attributeMask,
                        skinVertices, skinIndexSpace; };
struct SubmeshData    { indexOffset, indexCount, materialIndex, bonePalette; };
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

`interface/AYResource/IConverter.h`:

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

ITextureCompressor abstracts the currently available BC3 (squish) and BC7 (Basis Universal) encoders. The decoded-image import path rejects unsupported BC1/BC5/RGB8 requests instead of writing incorrectly labelled or oversized buffers.

### 5.5 Material → Phoskia shader reference

`MaterialData::shader` is the **virtual path to a Phoskia source file** (e.g. `shaders/pbr.phoskia`). The runtime bridge (`runtime-conventions.md` §1) reads the Phoskia text and hands it to `AYShader::ShaderResourcePool`. Material assets do **not** embed compiled shader binaries.

### 5.6 Status of source-format coverage

| Source | Parser | Mesh | Skeleton | Animation | Material | Texture | Notes |
|--------|--------|------|----------|-----------|----------|---------|-------|
| FBX | ✅ | ✅ | ✅ | ✅ (R-02 wired) | ✅ | ✅ | Primary path today |
| glTF 2.0 | 🔄 stub | 🔄 | 🔄 | 🔄 | 🔄 | 🔄 | R-04 in Phase 1 |
| PMX / VMD (MMD) | — | — | — | — | — | — | **Future frontend** — §5.7 (saba); not started |
| PNG/JPG/BMP/TGA | n/a | n/a | n/a | n/a | n/a | ✅ | |
| OGG/MP3/PCM | n/a | n/a | n/a | n/a | n/a | n/a | WAV→`.ayaudio` ✅; **MP3/OGG Dev+cook per §4.2.1 (pending A1–A4)** |

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
2. **Bone names on disk:** The future PMX/VMD frontend may **keep Japanese standard names** in `.ayskel`/`.ayanm` (the shared Player already binds by string). The planned pipeline preserves source names in authoring resources and compiles selected mappings/profiles into cleaned runtime outputs; see [skeletal resource pipeline](../../AYDocs/SKELETAL-ANIMATION-RESOURCE-PIPELINE.md). Import-time and editor-time configuration share the same core, never rename or guess aliases at tick time. Both the PMX/VMD frontend and the new publication gate remain future work.
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

### 5.8 骨骼作者资源与发布编译（2026-09-21，当前正式资源集已闭环）

设计权威入口为 [骨骼动画资源管线](../../AYDocs/SKELETAL-ANIMATION-RESOURCE-PIPELINE.md)，不改变 §7 中现有 `ISkeleton/IAnimation` 公共接口。
新增完整源骨架视图、SkeletonMapping、RetargetProfile 和 MappingTemplate 作者资源；允许用户
导入时不转换，在编辑器手工/模板配置后生成派生输出。映射按稳定资源/骨身份保存，不持久化 cook 索引。
默认配置绑定由编辑器元数据管理，避免 profile 与骨架的加载循环；完全烘焙输出不依赖作者模板。

当前 FBXParser 仍仅从 skin cluster 骨精简层级，因此完整源骨架与两根保留需要 SKA-03 的实质改造。
离线 bake 与 ProjectBuild 发布门禁现已覆盖当前正式文件资源：`.ayskel`、`.ayanm`、`.aymesh`
draw-local palette/有效 joint 与 `.aymask` 命名引用。清理只写派生资源，不删除源文件；删除骨仍被
有效蒙皮引用时阻止，未使用 palette 槽可压缩，动画轨道与 Mask 同步重写，不强制只留下 57 个角色。
当前没有独立挂点/IK/物理作者资源 schema；未来新增此类引用时必须先扩展闭包与重写器再允许发布。
跨骨架蒙皮网格需要几何空间 rebind，尚未实现时 `BakeToTarget` 会明确阻止，而不是只改 palette。

AYResource 负责 schema、IO、身份、构建记录及发布验收；清理、引用重写和转换数学由
AYAnimation 的 UI-free 作者核心提供。receipt v2 记录 scope、输出模式、platform、dry-run 依赖闭包、
源修订指纹和一一对应的产物表；ProjectBuild 在实际执行时复验源/目标/profile、完整闭包及所有输出，
排除作者源和不属于任何有效闭包的陈旧 `.baked.*` 产物。源/目标/profile/dependency 或输出变化会
阻止发布，不回退到旧缓存；烘焙采用暂存、反序列化验证、rollback 副本和最后切换 receipt，失败或
取消不替换整组有效输出。

## 6. L2 — Runtime resource manager

### 6.1 Loader contract

`interface/AYResource/IResourceLoader.h`:

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
| `AYResourceManager` | `include/AYResource/ResourceManager.h` | Public load/loadAsync/unload; resolves path → loader; owns cache |
| `AYResourceCache`   | `include/AYResource/ResourceCache.h`   | Strong + weak cache; LRU; thread-safe (`std::mutex`) |
| `AYResourceHandle<T>`| `include/AYResource/ResourceHandle.h`  | Ref-counted accessor; live handles LRU-protect the asset |
| `AYAsyncLoader`     | `include/AYResource/AsyncLoader.h`     | Real async (`std::promise`), progress callback, cancel |
| `AYHotReloadWatcher`| `include/AYResource/HotReloadWatcher.h`| Polls `mtime`; on change, invalidates handle, reloads on next access |

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

All L2 interfaces live in `interface/AYResource/assetsDefs/I*.h` and are **the public contract**.

### 7.1 `IAYMesh`

Source: `interface/AYResource/assetsDefs/IMesh.h`

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

Source: `interface/AYResource/assetsDefs/ISkeleton.h`

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

Source: `interface/AYResource/assetsDefs/IAnimation.h`

```cpp
enum class AnimTrackType : UInt8 { Vector3, Quaternion, Float };
enum class AnimInterpolation : UInt8 { Linear, Step, CubicHermite };

struct AnimTrack {
    std::string  nodeName;          // == skeleton bone name (or node name)
    std::string  property;          // "position" | "rotation" | "scale"
    AnimTrackType valueType;
    AnimBlendMode blendMode;
    AnimInterpolation interpolation;
    std::vector<Float32> times;     // source ticks
    std::vector<Float32> values;    // interpret per valueType
    std::vector<Float32> inTangents;  // optional; value units / second
    std::vector<Float32> outTangents; // optional; value units / second
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
    virtual AnimInterpolation getTrackInterpolation(UInt32) const = 0;
    virtual const Float32* getTrackInTangents(UInt32) const = 0;
    virtual const Float32* getTrackOutTangents(UInt32) const = 0;

    static constexpr UInt32 VERSION = 5;
    static constexpr UInt32 MAGIC   = 0x4E4D5941;   // 'AYNM'
};
```

`.ayanm` v5 adds one interpolation byte and two optional tangent arrays per
track. v1–v4 readers remain supported and produce `Linear` tracks with empty
tangents. Cubic arrays, when present, must exactly match the flattened value
array; malformed sizes are rejected on read and write rather than truncated.

### 7.4 `IAYMaterial`

Source: `interface/AYResource/assetsDefs/IMaterial.h`

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

Each is a thin, stable interface — see headers in `interface/AYResource/assetsDefs/`. Two cross-cutting notes:

- `IAudio` / `.ayaudio`: **ship = PCM**; Dev loose WAV/MP3/OGG decode policy locked in §4.2.1 (and `AYAudio/design.md` §2.3). Older “Format enum pick compressed at runtime” notes are superseded for v1.
- `IAYShader` is **legacy**; runtime uses `AYShader::ShaderResourcePool` directly with Phoskia source.

---

## 8. Public API surface (CR-enforced)

From `runtime-conventions.md` §8 + `ENGINE-FOUNDATION-PLAN.md` §4.1.

### 8.1 PUBLIC — propagated to dependents

```
AYResource.h
interface/**
interface/AYResource/assetsDefs/**
include/AYResource*.h
include/AYResource/AssetPath.h
include/AYResource/AsyncLoader.h
include/AYResource/HotReloadWatcher.h
include/AYResource/LooseDependency.h
```

Game / ECS / editor code includes `interface/AYResource/assetsDefs/I*.h` only. It links `AYResource` and calls `AYResourceManager::load<T>` / `loadAsync<T>` / `AYResourceHandle<T>`.

### 8.2 PRIVATE — this module + tests only

```
include/AYResource/assetsImpl/**   — concrete IResource classes (AYMesh, AYMaterial, …)
include/AYResource/Loader/**       — per-format loaders
include/Converter/**    — offline converters
```

A PR that exposes `AYResource/assetsImpl/Mesh.h` to another module **fails review**.

### 8.3 Cross-module include / link rules

(From `ENGINE-FOUNDATION-PLAN.md` §4.1.)

| Module | May include from AYResource | Must NOT include |
|--------|----------------------------|------------------|
| `AYEntity` (ECS) | `interface/AYResource/assetsDefs/I*.h` | `include/AYResource/assetsImpl/*`, `include/AYResource/Loader/*`, `include/Converter/*` |
| `AYRenderer` | `IAY*` (+ `assetsImpl` only inside `src/detail/` if cast needed) | n/a (owns L3 bridge) |
| `AYAnimation` | `interface/AYResource/assetsDefs/IAnimation.h` | `assetsImpl/*`, anything bgfx |
| `AYEditor` | `interface/**`, `AYResource/IConverter.h` / `AYResource/ImportJob.h`, `AYResource/ResourceBootstrap.h` | `include/AYResource/Loader/*`, `assetsImpl/*` |

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

### 10.1 后续骨骼动画管线实现项（非 Phase 0 完成声明）

- **P0**：SKA-01/03 作者资源与源保留，SKA-02 统一校验，SKA-05 构建指纹/状态，SKA-07 清理与全引用重写，SKA-08 发布 manifest/CLI 门禁；SKA-09 分项和闭环回归持续跟进。
- **P1**：SKA-10 批量/来源模板编排，SKA-13 版本兼容与配置迁移；协作 SKA-11/12 的转换输出。
- **P2**：协作 SKA-16 骨骼 LOD/平台压缩。

SKA-07/08 已针对当前正式骨架资源集完成；SKA-03、其余 P1/P2 项及未来新引用类型仍按 [统一队列](../../AYDocs/SKELETAL-ANIMATION-RESOURCE-PIPELINE.md) §7 推进，不改变历史 R-01/R-07/R-08 状态。

### 10.2 Project Build authoring contract（2026-09-20）

`AYResource::ProjectBuild` 是编辑器与命令行共享的 Build/Cook/Stage/Package
编排层。`ProjectBuildProfile::serialize/save` 现在提供唯一的规范化
`.aybuild.json` 写出路径，并通过临时文件加原子替换发布；读取、可视化编辑器与
`project_build_tool` 不再各自维护一套 JSON 结构。内容规则保持显式顺序，Pak
分块按 `pak:<chunk>` 规范化写出，Cook policy、缓存根目录、CMake 产物、打包和
运行参数均参与往返测试。

Windows 发布事务对系统错误 5/32/33（短暂目录/子文件占用）采用 10 ms 间隔、
250 ms 重试调度预算和最多 26 次尝试，覆盖发布、退役、恢复/回滚及备份清理。
单次底层文件系统调用本身不可由该预算中断；非 Windows 和其他错误不重试。
Cook 缓存目录发布同样复用有限重试。永久拒绝仍返回包含阶段、源/目标、
系统错误和次数的失败，绝不以复制替代原子改名。发布失败恢复旧包；恢复失败
保留 `.previous` 并显式报告，下次构建先恢复而非删除它。提交后旧备份清理失败
只报告警告，不将已经提交的新包误报失败。仍不支持同一输出目录的并发构建。

`.cookCache` 仍是内容寻址的派生数据，不是源资产。重复构建只在源指纹、转换参数、
平台配置与工具契约均未变化时复用结果；Raw、Cook、Exclude 与 Loose/Pak 是两个
独立维度。AYEditor 的 Project Settings & Build 窗口消费本契约，但后台执行器与
命令行保持可独立使用。

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
| State machine, IK, motion matching, retarget math | `AYAnimation` | Phase 2+；后续 SKA-02/06/11/12；作者资源、清理与发布编排仍由本模块/离线工具负责，见 §5.8 |
| glTF 2.0 real converter | `AYResource` | Phase 1 R-04 |
| PMX / VMD converters (`MMDConverter` + saba) | `AYResource` | Deferred — **spec in §5.7**; schedule with R-05/R-06 when batch MMD is needed |
| Pak/DB format, streaming | (future `AYStorage`) | Phase 4 |
| Scene / prefab format | `AYSerializer` (future) | now (JSON stub in old design §13) |

---

## 13. Revision history

| Date | Author | Change |
|------|--------|--------|
| 2026-09-21 | Content / animation | **SKA-07/08 当前正式资源集闭环**：事务式 bake 重写骨架、动画、网格 palette/joint 与 Mask；receipt v2 记录精确依赖/产物及源修订；ProjectBuild 在执行时复验 mapping/retarget scope、目标、profile 与完整闭包，排除作者源和陈旧 baked 产物。跨骨架蒙皮几何 rebind 未实现时显式阻止。AYResource 2495/2495。 |
| 2026-09-18 | Content / animation | **设计待实施**：§5.8 / §10.1 接入 SKA 队列；明确完整源骨架、映射/profile 作者资源、非破坏式配置、派生清理和发布门禁，并修正 MMD 映射仅限 Parser 的旧规划；无新增代码或功能完成声明。 |
| 2026-06-04 | Content team | Initial draft |
| 2026-06-09 | Content team | Added GUID system, SQLite schema drafts, precision compression framework, pak design |
| **2026-07-06** | **Content team** | **v2.0 — major realignment with `ENGINE-FOUNDATION-PLAN.md` v1.0 and `runtime-conventions.md`.** Trimmed speculative / out-of-scope sections (precision compression, full SQLite schema, pak design). Added: explicit non-goals, three-layer module layout, current vs target state, IntermediateAsset detail, L1↔L3 bridge map, CR-enforced public/private API surface, Phase 0 backlog with R-01/R-07/R-08 status, deferred items mapped to owning modules. Preserved all concrete API signatures from the actual headers — no API removed. |
| **2026-07-27** | Content / agent | **§5.7 MMD (PMX/VMD) via saba** — future optional Parser frontend reusing IntermediateAsset + existing typed Converters; CMake `AY_RESOURCE_USE_SABA`; near-term path remains Blender→FBX. Updated §5.6 coverage table + §12 deferred row. |
| **2026-08-13** | Audio / agent | **§4.2.1 Audio authoring vs cook locked** — Dev loose WAV/MP3/OGG in AYResource; cook PCM `.ayaudio` only for ship/optimize; aligns `AYAudio/design.md` §2.3. Updated `.ayaudio` row + §5.6 + §7.5. |
| **2026-08-02** | Content / agent | **P6 ownership** — §6.5 hot-reload matches eager reload + `setOnHotReload`; §8/§11 PublicApiSurface scan + allowlist; link `ownership-contracts.md`. |
## Tilemap runtime v3（2026-09-10）

`.aytilemap.json` 是可编辑作者文件，`.aytilemap` 是运行时资源。v3 保留 v1/v2
读取兼容，并把编辑器已经具有、但 v2 无法表达的数据纳入同一份运行时资源：

- layer 0 继续占用旧 tile-id 主数组，额外层以相同 pack mode 顺序存储；每层携带
  visible，名称、文件夹、stamp 和 terrain rule 仍只属于作者数据；
- 图集表保存来源路径与原始尺寸，Tile visual 表保存 atlasId、精确像素裁剪矩形和
  `0xRRGGBBAA` tint；运行时不再从 Tile ID 猜测规则网格位置；
- 阴影保存为每格四分区语义 mask 和全图颜色，不烘焙进 PNG；动画表和碰撞表沿用
  v2 契约；
- Converter 以最终二进制作为 GUID 摘要输入，任意图层、裁剪、色调或阴影变更都会
  使 cook cache 失效。

图集可移植化补充（2026-09-11）：Converter 将作者图集无损烘焙为单 mip RGBA8
`.aytex`，运行时 Tilemap 只保存 `textures/...` 虚拟路径；同时在主
`.aytilemap` 旁原子更新同名 `.aydep.json`，使 CookShip 能把 Tilemap→Texture
边写进 `resources.db`。CookShip 与 loose dependency resolver 都把标准资源目录
视为 Assets 根相对路径，不能把 `tilemaps/level.aytilemap` 引用的
`textures/atlas.aytex` 拼成 `tilemaps/textures/...`。即使侧车缺失，ITilemap
内置 atlas catalogue 仍作为运行时 intrinsic dependency，保证 loose-file
加载保持可用；侧车只承担离线打包图和预加载顺序。CookShip 严格遵循 cooked-tree
契约，只收录最终 `.ay*` 资源；Editor/Debug 中注册用于快速迭代的 PNG、WAV 等松散
源格式，以及作者 JSON 和依赖侧车，都不会进入发布包。

选择这一边界的原因是：terrain/stamp 是生成 Tile ID 的编辑操作，文件夹/名称是资产
组织信息，它们不影响运行；layer、visual、tint、shadow 则直接影响每帧绘制，必须进入
运行时格式。v1/v2 资源在接口中自然表现为“一个可见层、无视觉目录、无语义阴影”。

## ProjectBuild Logia 依赖发布（2026-10-06）

AYProjectBuild 的可选作者依赖由 AYScript 配置阶段连接，避免 AYResource runtime 引入脚本依赖。
ProjectLogiaContent 复用 Logia tokenizer/parser 与 expandLogiaModules，不复制语言规则；这里验证
语法、helper-only import 和闭包安全，不假设各入口的 Actor schema/provider，因此不替代宿主语义编译。
计划收集并排序 transitive scriptDependencies；执行在创建 staging 前重读并比较，新增/删除 import
要求重新计划，失败保留已有发布目录。未匹配规则的 .logia 强制 raw/loose，显式不兼容规则是硬错误。
发布清单列出便携依赖并写 DB dependency edge。ProjectLogiaTests 移走作者 Assets 后从 Content 运行递归 import。
2026-10-07 补充异步发布验收：Content 中的 import helper 接收已检查 task 参数，并在后续 tick 消费
task_complete 的 int/bool payload；作者 Assets 移走后流程仍完整运行，不引入新的发布格式。

Logia behavior composition is performed by AYScript module expansion. ProjectBuild
uses that bounded closure for source edges and publication, allowing instance
state/persist fields and hooks but retaining shared-export/path/cycle rejection.
Host-specific property/provider semantics are still checked by runtime loading.
## Optional deterministic levels (Stage45)

The AYEntity-owned resource target depends on Resource+DeterminismKernel; Resource Core
has metadata-only `AY_RESOURCE_HAS_DET_LEVEL` and retains its dependency boundary.
Authoring Import/ProjectBuild/Cook targets alone gain the kernel when the opt-in is on.
The converter implementation is explicitly compiled into Import, never runtime Core.
The default off configuration and all pre-existing Id numeric values are preserved.

`.dlt` is bounded source text; `.dlc` carries AYDLC001 canonical validated Level text.
The optional loader publishes immutable shared snapshots only after whole-buffer
validation. Failure retains the previous snapshot; an externally retained snapshot
outlives resource unload. Registration is quiescent, explicit and idempotent for the
same type, and rejects suffix/type conflicts. Async loader is synchronous completion
under the existing loader interface, not an added concurrent reload guarantee.

Converter output is root-relative, canonical `.dlc`, typeDeterministicLevel2D and one
resource with no dependencies. Its contract tag enters the Project SHA key. Single
imports skip timestamp caches only for `.dlt`; other importer behavior is unchanged.
Before staging Project output, contract/type/path/count/dependencies are checked and
the complete decoded cached canonical Level is compared to the current source after
recomputing its plan key. A valid checksum alone is insufficient cache identity.
Source mutation after plan and invalid Raw/authoring publication fail before replacement.
CookShip bounds and semantically validates each final `.dlc` before opening replacement
Pak/database outputs. FNV is damage detection, not authentication or Session compatibility.

Windows cache filesystem operations use native extended paths while importer
outputDir keeps ordinary logical spelling. The Level converter independently
reapplies native spelling at every physical boundary after canonicalization;
containment and resource metadata use ordinary identity paths. This preserves
legacy converters while allowing deep Level staging/publication/replacement/reuse.
The302-character directed gate and affected legacy Audio SHA cache passed in
Release. The deep copy branch has no separate coverage claim; native acceptance
is deferred. Original failed attempts remain in the execution evidence.

## Extended diagnostic observations (2026-10-15)

ResourceManager::debugSnapshot copies load state/progress and CPU cache stats/residency without loading, LRU promotion or callback replacement. State and cache locks are separate, so the result is not a globally atomic transaction. Reload events include a lifetime-monotonic diagnostic sequence so repeated reloads of one path remain distinct. Load/resident rows cap512, optional progress keys512 and reload history64. progress=-1 means unavailable; cachedOnFailure means a failed path also had a cache entry at the observed cache snapshot, not proof of a particular placeholder type. Progress/reload capture defaults off and preserves user callbacks; clear affects only diagnostic history.

Regression sources are included. Runtime tests and interactive editor acceptance are deferred for this content task; untested changes remain uncommitted.

## Compiled audio authoring content (2026-10-16)

AudioContentConverter supports .performance.json and .chart.json through the
standard converter factory and ImportJob. Models/renderer live under
include/AYResource/authoring; AYAudio editor compatibility headers forward here.
The runtime AudioContent loader reads only .acb v1: validated PCM plus sorted
source-frame event metadata, with no author paths or external sample dependency.
AudioContent derives from Audio/IAudio, so existing PCM registration remains usable.

ProjectBuild requires Cook for these source types, rejects missing/escaping/
excluded project samples, hashes their closure (including built-in piano .aud),
rechecks inputs before execution, validates cached output identity/payload and
publishes loose or Pak. Native Windows paths normalize separators before .acb
reads, including extended long cache paths. Built-in piano data is resolved from
adjacent tool/editor assets first, then the configured engine sample directory.
Derived piano attribution is embedded and published as licenses/audio-content.txt.

Limits: fixed BPM; <=64 tracks/4096 source clips/100000 compiled events;
<=600 seconds/256 MiB PCM, <=16 MiB metadata, <=64 KiB credits. Loading rejects
malformed/truncated/range-invalid bundles without replacing existing content.
Runtime playback/owner lifetime is the AudioContentPlayer facade in AYAudio.
See AYDocs/sdk/contracts/audio.md and unittest/AYTest_AudioContent.cpp for the
Cook/cache/Pak/author-tree-removal regression evidence.

Validation (Windows Debug, 2026-10-16): AudioContentTests passed 3/3 (48 checks),
including actual piano Cook/attribution, content-addressed cache replacement,
stale-plan rejection and Pak loading after moving the authoring Assets tree.
Affected ProjectBuild/import/bootstrap/decoder/loader suites passed 49/49
(472 checks). Standard import_tool cooked a piano arrangement successfully.
Reports: out/audio-completion-20261016/content-credits-final.json and
resource-integration-final.json. AYAssetFormat_Tests and both editor builds passed.

## Room assembly source-to-package route (2026-10-26)

The optional AY2D authoring integration registers `.ayrooms.json` with the
generic importer and `.rmb` with the runtime resource registry. The converter
audits the authored seed and emits one AYRM v1 envelope at the source-relative
path. ProjectBuild includes the converter contract in cache identity, rechecks
the source before execution, and validates cached output path/type and bytes.
CookShip validates final envelopes before replacing an existing package. Game
state and generated room objects remain outside the asset pipeline.
