# AYResource

## 对话作者 JSON 发布

启用 AYProgression JSON 与资源作者构建后，ProjectBuild 识别所选的
`.dialogue.json`，只接受 Raw 规则。规划时复用 AYProgression 的 1 MiB 有界解码与
对话图校验，并拒绝同一项目中重复的文档 ID；执行时重验原始内容，暂存前冻结
一份副本并与规划时 SHA-256 身份核对，失败保留旧包。可以按项目规则选择 loose
或 Pak 存储；运行时读取仍由游戏负责。这一步只保证文档结构与原始字节一致，
未提供项目映射时不解析语言、语音和 Sequence 键的文件闭包。

项目可在 Build Profile 的 `content.dialogueBindings` 填一个 Assets 相对 JSON
路径，显式开启跨文件发布门禁。例如：

```json
{"schema":1,
 "documents":["dialogue/intro.dialogue.json"],
 "locales":["locales/en-US.json","locales/zh-CN.json"],
 "voice":{"voice.intro":"audio/intro.aud"},
 "sequence":{"sequence.intro":"scenes/intro.sqc"}}
```

映射文件与全部引用文件必须被本次构建选中为 Raw 普通文件；所有选中的对话文档
必须列出。每种语言文件须提供全部说话人、正文与选项键，不用回退语言补缺。
Voice 指向可解码的 `.aud`（单件上限 128 MiB）；Sequence 指向已编译且依赖闭包
有效的 `.sqc`，需要 AYSequence 作者构建。规划、执行和暂存使用同一内容身份；
缺键、排除、错误类型或规划后变更均拒绝发布。映射是项目显式提供的键到文件关系，
不替游戏决定具体台词、语音或镜头资源。
`AYProgression::Json` 提供同一映射的 `decodeBindingsJson` 与 cue 路径查询；
游戏可读取随包发布的 JSON 字节后直接复用该解析器。
包内语言 JSON 还可交给 `Localization::loadFromJson`；ProjectBuild 用例以
Pak 里的文档、绑定、语言联合验证文本显示与读档不重播媒体。宿主仍负责选择
语言以及将 Voice/Sequence 路径交给媒体播放器。

## 可选场景序列发布（2026-10-26）

启用 AYSequence 作者构建后，ProjectBuild 把显式 Cook 的 `.seq` 发布为
`.sqc`，将 Scene/Animation/Audio 引用作为单独资源留在原 Assets 相对路径。
规划与执行阶段检查引用是否存在、是否被排除或转换，依赖字节进入缓存键；
暂存后再核对 `.sqc` 签名和实际发布闭包。CookShip 拒绝损坏的 `.sqc` 或
缺少依赖的输入树，失败不会替换既有包。普通运行时 AYResource 不引入
AYSequence Core；加载器由 AYSequence::Resources 可选注册。

## 作者刷新结果（2026-10-22）

`ResourceManager::refreshPublishedResourcesWithReport`保留既有CPU批次准备/替换规则，
再返回每条规范路径的消费者结果。`cpuRefreshed`只表示CPU阶段完成；
`Updated`、`Retained`、`NotResident`、`NotHandled`、`Unconfirmed`、`Failed`分别表达消费者状态。
文件发布、CPU替换和GPU更新仍为不同事务。报告只持有值，不延长任何GPU资源寿命。

高级Host用`setRefreshObserver`替换现有单一通知slot，并保存注册token；
关闭时调用`clearRefreshObserver(token)`，不会清掉后来注册的回调。
旧`setOnHotReload`与bool刷新入口保留；void回调结果标为`Unconfirmed`。
回调复制后调用，允许通知内替换；异常标为`Failed`，不能推断消费者已完全回滚。
所有注册、通知与清理仍在submission owner线程，没有新增并发或多消费者总线。
诊断/标签及每路径结果数量有界；限制与CPU/GPU契约见[所有权说明](docs/ownership-contracts.md)。
通用实现8例/100断言、完整模块384项中372通过/12项可选FBX夹具跳过、4631断言通过。
六个相关ASan套件64例/486断言通过。Renderer两后端及实际Editor拒绝/修复通过；
Editor后补来源隔离另行验收，不与上述通用Resource结果混算。

## 纹理采样与容器兼容（2026-10-22）

Texture writer 写 `.txr` v2：36字节旧头、16字节 sampler、原编码 mip 数据；读取器兼容 v1并恢复历史默认采样。
过滤、各轴寻址、各向异性、sRGB 与 LOD bias 均可往返；具体 GPU 支持及回退由 AYRenderer 决定。
编解码检查有效枚举/有限数、尺寸/层级、精确布局及256 MiB数据上限；失败加载保留旧对象，失败保存保留输出缓冲。
`TextureSampling.h` 只包含轻量采样值，不依赖资源管理器或 GPU。

作者通过 ImportOptions 的 `overrideTextureSampler` / `textureSampler` 和 `keepTextureMipmaps` 设置输出。
覆盖在 staging 对 typed 纹理生效，保留来源 sRGB；关闭层级保留会裁去基础层之后的数据。
不为 DDS 生成缺失层级；raw 图片无法保存这些设置，需要选择 cooked 输出。
共享 recipe v1以可选字段保存设置；设置变化参与缓存指纹、批量导入和 ProjectBuild。
未覆盖的旧配方保持原语义。加载 CPU sampler 不意味着设备端会即时更新，发布/驻留刷新仍由 Host 显式驱动。

## 导入与重导入（2026-10-21）

作者工具使用 `ImportJob.h` 的 `importAsset`，FBX/glTF/GLB 共用真实 Assimp 场景解析与类型 writer。
同一输出根的导入由调用方串行执行。转换先写空 staging，验证来源快照与输出后发布；
失败回滚已有文件。多文件发布不承诺进程崩溃原子性或并发读者的统一快照。
Windows 发布及回滚复用 AYIO 的有界文件操作重试（最多26次、约250毫秒），容忍扫描读者造成的
短暂共享/锁定/访问冲突；持续错误仍失败，错误包含操作路径、原生错误码、次数及预算耗尽信息。

模型/图片缓存校验来源、实际外部依赖、完整设置和输出的 SHA-256；旧无指纹缓存需重建，
显式 `allowLegacyCache` 只用于兼容既有工具。来源身份与可选输出命名空间可隔离同名模型及贴图。
项目作者配方存放在 `.ayeditor/imports/source-*.json`，不属于派生缓存；
`readProjectImportRecipes` 为 Editor/ProjectBuild 共享读取入口，项目相对引用支持目录迁移。
ProjectBuild 共用这些设置，并在规划时发现模型实际读取的外部 buffer/图片；
这些输入与配方参与 cook key，规划后变动须重建计划。构建 profile 决定发布输出及贴图 cooking。
同源重导入保持输出命名空间；安全旧贴图名保留，父目录引用和扁平化冲突使用确定的派生名。
配方不是运行时资源，sourceId 也不是安全凭证。
项目新配方固定 outputBaseName；重新指定同扩展名源文件时保留它与 namespace/sourceId，
因此主文件改名/搬目录不会改变既有网格/材质/骨架/动画或独立图片的输出路径。
模型内部子对象改名、外部图片 URI 改名和贴图输出模式变化仍可能改变对应子资源路径。
旧配方的可选 outputBaseName 缺省为空，编辑器在重新指定来源前记住原文件 stem；作者格式仍 v1。



glTF Auto 使用规范的右手、Y-up、米制基准，并保留显式 metersPerUnit 覆盖；Manual 使用用户轴向。
材质槽、AO、金属 B/粗糙度 G 和内嵌图片按真实来源转换。DDS 单张 2D 的 UNORM BC1/3/4/5/7
封装为可读取的 .txr，保留编码块和 mip；拒绝截断、数组、立方体、体积及其他格式。
Renderer资源桥现已上传完整mip链；有效但不完整的链明确回退基础级，单层不自动生成层级。
具体设备格式/采样回退由Renderer决定，CPU保存成功仍不等于驻留GPU刷新成功。
音频作者内容、Particle、Stats、确定性 Level 保留各自既有闭包重校验/禁用单次导入缓存策略；
不能将模型/图片的缓存命中保证推广到所有领域。离线 ProjectBuild 继续负责这些领域的闭包 SHA 缓存。

离线工具同样消费作者设置：import_tool --project <project> --list-recipes，随后用
--project <project> --recipe <source-id> 执行，可附加 --force 或 --out <alternate>。
不允许混入 --in 或局部设置覆盖；不会通知另一个编辑器进程的 GPU。

`refreshPublishedResources` 在主线程暂存已驻留 L2 的新资源，再替换缓存并通知现有 GPU observer。
刷新包含报告路径、weakly_canonical 物理路径，以及类型表中已知、指向同一物理文件的
绝对路径别名；因此配方用物理路径、Renderer 经目录联接加载的情况也能更新。
类型键在锁内复制，文件系统检查在锁外进行。L2 淘汰后的已知别名也通知 GPU 消费者；发现阶段
不主动加载已淘汰条目，observer 可按需加载。显式 unloadResource 会忘记该路径身份。
所有驻留拼写在通知前准备好
新 L2，输入去重，旧借用对象继续有效。此处不合并全局缓存身份，不搜索文件系统上的未知别名，
也不改写包数据库虚拟键。规范化不是通用的硬链接身份或全部 Unicode 大小写等价判断。
解码失败保持旧缓存且不通知；已经借出的旧 shared_ptr 仍有效。磁盘发布与运行时刷新是两个步骤。
UTF-8 的源场景/外部 buffer 和输出路径已有定向检查；尚未保证所有旧运行时/图像 API 的 Unicode 路径。

## 单材质读取边界（2026-10-20）

`.mat/.aymat` 单材质 decoder 先构造临时值，完整验证 v1/v2/v3 负载后再替换当前对象。
拒绝截断/额外字节、错误数量/tag、空/重复/NUL 参数名和非法 bool，
Matrix 保留完整 16 个 float；name/shader 也拒绝嵌入 NUL。
失败保留现有值，保存拒绝超过单文件头可表示的 255 个参数。
多材质 bundle 继续使用 MaterialFile；IMaterial ABI3 与文件版本3不变。
这些是既有格式的结构校验，通用 Float payload 不承担材质编辑器的数值范围策略。

## 默认项目 shader 发布（2026-10-05）

启用 AYShader 的作者工具下，ProjectBuild 默认校验并暂存代码产物旁的 engine shader 包，
收集选中的 raw/loose `.phoskia` 程序和模块，离线生成 project 包；无需调用方新增构建步骤。
纯 helper 模块不独立生成程序，缺失/排除模块、错误源、Pak/Cook 选择或不支持的跨目标均阻止发布，
保留原发布目录。engine/project 包列入包清单，并生成 `Shaders/production.policy`，
使发布 Debug 也不在启动时依赖 shaderc。源码仍 loose 以支持正常文件/模块解析。
默认只生成无宏变体，附加宏需显式 shader 构建清单；无 AYShader 的作者构建明确拒绝该能力。

AYResource 是资产注册、导入、加载、缓存、热重载与 Cook/Ship 管线模块，维护资源 GUID、虚拟路径和运行时资产接口。

## 角色资产规范

标准资产条件见 [AYHumanoid 骨架与模型规范](../../AYDocs/AYHUMANOID-STANDARD.md)，FBX 操作见 [外部角色与动画导入标准](docs/external-character-animation-import-standard.md)。参考资产尚待按 [low poly 实施计划](../../AYDocs/AYHUMANOID-IMPLEMENTATION-PLAN.md) 制作。

## 公开接口

```cpp
#include <AYResource.h>
#include <AYResource/IResource.h>
#include <AYResource/IResourceLoader.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/ImportJob.h>
```

资产接口位于 `interface/AYResource/`，管理器和管线头位于 `include/AYResource/`。

## 依赖

- 公开：AYMath、AYSerializer、AYIO、AYStorage、AYTask、AYEventSystem
- 内部：AYLog、Assimp、stb、libsquish、basisu

资产格式、导入/Cook 和缓存契约见 [design.md](design.md)。

测试筛选入口、fast/integration 分层及已知发布问题见[统一测试契约](../../AYDocs/testing.md)。

## ProjectBuild 的 Logia import 闭包

启用 AYScript 的作者构建自动解析所选 .logia 的 helper import，记录 Assets 相对的递归依赖。
未匹配显式规则的脚本自动选择 raw/loose；显式 excluded、pak/cook 依赖会使计划失败。
计划与执行均校验缺失、循环、根逃逸、helper 限制及数量/大小预算；执行时闭包变化要求重新计划。
依赖写入计划 JSON、发布 manifest 和 resources.db，不包含作者绝对路径。语义/Actor schema 仍由脚本宿主校验。
禁用 AYScript 的作者构建遇到需发布的 .logia 会明确报错。验证见 `ProjectLogiaTests`。
该套件也验证发布的 import helper 在移走作者 Assets 后接收 task 参数、等待 typed EventBus payload 并更新 Actor。

## Logia resource dependencies

Checked 脚本以 `asset sound:string="audio/start.ayaudio"` 声明 immutable 的 asset-root-relative 路径。
编译器/CLI analysis 记录 resourceDependencies；import 模块也可导出 asset 常量。
普通字符串保持动态语义，不自动收集。运行时允许 DB/pak 虚拟资源；路径以 AYIO PathSafety 检查根边界。
脚本包 v2 原样复制声明资源并记录内容指纹，读取器兼容无资源的 v1；单资源 64 MiB，总资源 256 MiB，最多 128 项。
缺失资源、指纹不符、路径越界、清单冲突拒绝发布/加载。包只复制资源，不替代 cook；目标 Host 仍须有对应 loader。
ProjectBuild 的 resourceDependencies 独立记录资源边，使用项目 transform/storage 规则，排除/缺失引用为错误。
执行前重新检查依赖集合，变更须重新规划；失败不替换上一份输出。示例：[resource_refs.logia](../AYScript/examples/resource_refs.logia)。

Logia ProjectBuild closure validation accepts checked reusable behaviors with
instance fields/signals/lifecycle hooks; shared var exports remain invalid. Sources
stay raw/loose and dependency sets are rechecked before publishing. The
ProjectLogiaTests stateful_behavior_hooks_survive_project_publication case runs
the published behavior without its original authoring paths.

## Optional deterministic level content

`AY_ENTITY_LEVEL_RESOURCES=ON` adds the AYEntity-owned adapter and offline converter.
Runtime Resource Core receives `.dlc` packaging metadata only and does not link a
Session/kernel. The separate `AYEntity::DeterministicLevelResources` target loads
immutable validated initial-level snapshots through the existing ResourceManager,
including Pak bytes. Startup explicitly calls `initializeLoaders`, then
`registerDetLevelResources2D` before concurrent loads; metadata registration alone
does not provide that loader. Level acquisition/configuration stays outside Sim.

With resource authoring enabled, `.dlt` imports produce one `.dlc` preserving its
source-root logical subdirectory. Single imports revalidate complete content instead
of using timestamp sidecars. ProjectBuild retains its SHA content-addressed cache,
but also validates the sole output/type/importer contract and exact current source
canonical content before publication. Changed plans, damaged or valid-but-different
cached levels are refused. CookShip validates final `.dlc` before replacing its
previous package/database; keep source/cooked trees quiescent during publication.
Raw `.dlt` is authoring and is not accepted as a shipped runtime resource.

Build/use instructions and runnable ResourceManager example are in
[AYEntity's level-resource example](../AYEntity/examples/detlevel-resource/README.md).
This opt-in does not change ordinary resource types, install a Session/World/Scene,
or hot-reload running deterministic authority.

Necessary local Windows Release integration is passed: three resource paths and
the affected bootstrap, existing mesh Pak, timestamp sidecar, raw storage and
Audio SHA-cache cases. Physical extended paths stay at filesystem boundaries;
the public importer output directory remains its original logical spelling.
The new level cache gate retains a302-character object and checks replacement
and reuse without shortening its path. Native final acceptance is deferred.

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

## 2D room recipe build integration (2026-10-26)

When AY2D authoring and AYResource authoring targets are enabled together,
ProjectBuild selects `.ayrooms.json` for automatic Cook into `.rmb`, stores the
result in its content-addressed cache and registers the cooked
`RoomAssembly2D` type. Invalid sources and cached envelopes are refused before
publication. CookShip validates `.rmb` before packing. The runtime loader is
provided by optional `AY2DRoomResources` registration.

具体 Material::setTextureReference 供作者工具重定位既有纹理引用，保留 Texture2D/3D/Cube 类型；
拒绝缺失、非纹理参数及 NUL 路径，不改变失败前的值。它不是新的运行时资源获取入口。
证据 MaterialFileTests，普通与 MSVC ASan 材质文件组均通过7例/357断言。
