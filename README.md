# AYResource

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
