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