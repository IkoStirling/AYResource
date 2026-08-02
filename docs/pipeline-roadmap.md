# AYResource — 完整资源管线路图（P0–P6）

**状态**: P0–P6 ✅（路线图收束）  
**日期**: 2026-08-02  
**范围**: 架构一致性与端到端闭环，**不是**再堆具体资源类型（`.aymesh` / `.ayanm` 等）。

---

## 1. 现状一句话

有两套半入口、依赖解析分层断裂、热更/打包/流式未闭环；动画 / Entity 走 `ResourceManager`，渲染绕开它直调 `ResourceRegistry`。

```
Offline:  FBX → IntermediateAsset → Converters → .ay* + .aydep.json   ✅ 较成熟
Runtime:  ResourceManager (cache/deps/async/DB/pak)                    ✅ 骨架在，消费不一致
Bind:     RenderAssetBridge → GPU                                      ✅ 有，但引用解析偏晚
```

三层分工（已定，见 `design.md`）：

| 层 | 谁管 | 例子 |
|----|------|------|
| **L1 磁盘** | `AYResource` | `.aymesh`、`.ayskel`、`.ayanm`、`.aytilemap` |
| **L2 CPU 运行时** | `AYResource` | `IMesh`、`ISkeleton`、`IAnimation`、`IAYTilemap` |
| **L3 GPU** | `AYRenderer` | `RenderMesh`、纹理 upload |
| **领域逻辑** | 各模块 | `AnimationPlayer`、`World2D` / Tilemap |
| **场景图持久化** | `AYEntity` + `AYSerializer` | `.ayscene`（**不在** AYResource） |

---

## 2. 优先级清单

### P0 — 统一运行时入口（否则谈不上完整管线）

| 问题 | 影响 |
|------|------|
| `RenderResourceManager` 用 `ResourceRegistry::loadByPath`，**绕过** `ResourceManager` | L2 缓存、依赖预载、pak/DB、热更、内存预算对渲染路径失效 |
| 动画 / `AYEntity` 走 `ResourceManager`，渲染走 Registry | 同资源可双份解析、双份生命周期 |

**要做的事**：

- 规定唯一入口：`ResourceManager::load` / `loadAsync`（或 `ResourceHandle`）
- `AYRenderer::RenderResourceManager::loadMesh` / `loadMaterial` / `loadTexture` 改为走 Manager
- 渲染只消费已加载的 `IMesh` / `IMaterial` / `ITexture`，再上传 L3
- `ResourceRegistry` 降为 Manager 内部实现细节（对外不再作为加载入口）

**证据路径**：

- `AYRuntime/AYRenderer/src/detail/RenderResourceManager.cpp`（`loadMesh` / `loadMaterial` / `loadTexture` → `ResourceRegistry::loadByPath`）
- `AYRuntime/AYResource/include/AYResourceManager.h`

**验收**：渲染路径加载的 mesh/material/texture 出现在 `ResourceManager` 缓存中；热更 / unload 能触及同一份 L2。

---

### P1 — 依赖图与加载语义闭环 ✅

**已做（2026-08-02）**：

- `collectIntrinsicDependencies`：mesh slots → mat、material texture params → tex（无需 sidecar）
- `_loadInternal`：sidecar + intrinsic 递归预载；`_loadingPaths` 防环
- `ResourceLoadState`（NotLoaded/Loading/Ready/Failed）+ 缺失 `.aymat`/`.aytex` 占位（default mat / magenta 1×1）
- `createHandle` 改为走 `_loadInternal`（与 sync/async 同图）

**仍留给后续**：并行图调度、非 `.ay*` 图像路径的占位、FBX 根级 `.aydep.json` 与 per-asset sidecar 对齐（P4/P5 可顺带）。

---

### P2 — 热重载端到端 ✅

**已做（2026-08-02）**：

- `HotReloadWatcher` → AYIO `FileWatcher` + mtime fallback + debounce
- Manager：load 后 auto-watch；变更时 unload→`_loadInternal`；`setOnHotReload` 为 post-L2 通知
- `ResourceHandle::get` 检测 cache 实例替换
- Renderer：`pollResourceHotReload` + `onResourceFileChanged`（mesh/mat/tex 保 handle id 重传）
- Shader→material 仍走既有 `pollShaderHotReload`（AYShader 独立路径）

**仍留给后续**：反向依赖扇出（tex 变更刷新引用它的 mat）、pak 内资源热更、AY2D tilemap 订阅。

---

### P3 — 异步与缓存硬化 ✅

**已做（2026-08-02）**：

- `AsyncLoader` → `AYTask` `defaultScheduler`（无 per-job `std::thread`）
- typed `loadAsync<T>` 在 worker 回调里 cast（去掉 `castResourceFuture` 第二线程）
- `ResourceCache`：LRU demote → grace/weak；`get` 可 resurrect；`CacheStats` 可观测
- `savePersistentCache` / `loadPersistentCache`：轻量 residency 索引（`AYCACHE 1`）

**仍留给后续**：真正序列化 L2 blob 到二级缓存目录、按压力自动 trim 策略调参、AsyncLoader 可注入自定义 scheduler。

---

### P4 — Cook / Build + DB / pak 出货路径 ✅

**已做（2026-08-02）**：

- **库核心** `AYCookShip`（`cookShipPackage`）：扫描 cooked `.ay*` → `content.pak` + `resources.db`（含 `.aydep` 依赖）
- **工具 exe** `AYTool/cook_tool`（薄 CLI；`package_tool`/`index_tool` 路径修复；根 CMake 接入）
- **运行时** `ResourceManager::openDatabase`；相对 `in_package` 相对 DB 目录解析；DB/pak 失败 → loose fallback

**约定**：离线 exe 只放 AYTool；打包/索引/转换核心仍在 AYStorage / AYResource。

**仍留给后续 / P5**：批量 FBX→`.ay*` 导入编排、Editor Build UI、index_tool 与 StorageDatabase 统一。

---

### P5 — Editor / CLI 导入编排 ✅

**要做的事**：

- 统一 import 编排（FBX → Intermediate → converters → cache 目录）
- 进度 / 取消 / 错误报告对 Editor 可用
- 与现有 `EditorShellDemo` 默认 import 路径对齐

**落地**：

- **库核心** `AYImportJob`（`importAsset` / `importAssetBatch` + `ImportProgress` / `ImportCancelToken`）
- **工具 exe** `AYTool/import_tool`（薄 CLI；`--in` / `--out` / `--force`；支持 batch）
- **Editor** `ayt::editor::Importer` 改为调用 `importAsset`；默认仍写 `ayeditor_cache/assets\`（`--import` / Sour.fbx 不变）

---

### P6 — 跨模块 load / own / unload 契约 ✅

**要做的事**：

- 明确谁持有 L2 `shared_ptr`、谁持有 L3 handle
- unload / trim 时的跨模块通知约定
- 文档化「游戏代码只 include `interface/assetsDefs/IAY*.h`」的强制边界（CR / 测试已有部分）

**落地**：

- 契约文档 `docs/ownership-contracts.md`（L2 cache/handle vs L3 RRM；unload/trim 不通知 L3；仅热更 `setOnHotReload`）
- API 注释：`ResourceManager` unload/trim/hot-reload；`RenderResourceManager` L3 寿命
- 测试：`AYTest_ResourcePipelineP6` + `PublicApiSurface` 扫描（allowlist：`docs/private-include-allowlist.txt`）

---

## 3. 建议执行顺序

```
P0 统一入口
  → P1 依赖图 + placeholder/failure
    → P2 热更 E2E
      → P3 异步/缓存硬化
        → P4 cook/pak 出货
          → P5 Editor/CLI 编排（可与 P4 部分并行）
            → P6 跨模块契约收束
```

P0 是工业级管线的前提；在 P0 完成前，不要并行大开 P4/P5 新入口。

---

## 4. 明确不做（本路线图）

- 把 L1/L2 管线拆到各玩法模块（`.ayanm` 不进 AYAnimation，`.aytilemap` 不进 AY2D 独享）
- 为「完整管线」再堆一批新资源类型（类型按产品需求另开任务）
- 场景 / prefab 格式（属 `AYSerializer` / `AYEntity`）

---

## 5. 进度

| 项 | 状态 | 备注 |
|----|------|------|
| P0 统一运行时入口 | ✅ | `loadMesh` / `loadMaterial` / `loadTexture` → `ResourceManager` |
| P1 依赖图与加载语义 | ✅ | intrinsic deps + LoadState + placeholders |
| P2 热重载 E2E | ✅ | FileWatcher + L2 invalidate + L3 re-upload |
| P3 异步与缓存硬化 | ✅ | AYTask pool + grace resurrect + cache index |
| P4 Cook/DB/pak | ✅ | AYCookShip + cook_tool + openDatabase |
| P5 Editor/CLI 编排 | ✅ | AYImportJob + import_tool + Editor façade |
| P6 跨模块契约 | ✅ | ownership-contracts + PublicApiSurface scan |

---

## 6. 相关文档

- `AYResource/design.md` — 三层模型与 API
- `AYResource/docs/runtime-conventions.md` — 运行时约定
- `AYResource/docs/ownership-contracts.md` — L2/L3 持有与 unload/热更契约（P6）
- `AYResource/docs/phase0-follow-ups.md` — Phase 0 遗留 F-01..F-03
