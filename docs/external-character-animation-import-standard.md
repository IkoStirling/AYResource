# AYEngine 外部角色与动画资产导入标准（v1）

本文是 DCC、外包和资产商向 AYEngine 交付角色资产的最低契约。模型与动画文件承担不同职责；不要为了加入动画而重新导出并替换一个已经验证通过的模型文件。

## 1. 交付物

每个角色至少包含：

- `<Character>_Model.fbx`：网格、材质、纹理引用、目标骨架和 bind pose 的权威来源。
- `<Character>_Anim_<Clip>.fbx`：一个或多个烘焙动画 clip；不作为网格、材质或目标骨架来源。
- `Textures/`：模型 FBX 引用的图片。允许 PNG、TGA、JPG；路径必须相对交付目录。

EditorShellDemo 对应参数：

```powershell
AYEditorShell_Demo.exe --import D:\Assets\Hero_Model.fbx --animation D:\Assets\Hero_Anim_Idle.fbx
```

离线导入动画：

```powershell
import_tool.exe --in D:\Assets\Hero_Anim_Idle.fbx --out D:\Cache\assets --animation-only
```

`--animation-only` 只生成 `.ayanm`，不会生成动画文件里附带的网格、材质、纹理、碰撞体或描边壳。

## 2. 坐标、单位与面绕序

导入后的引擎空间固定为：

- 左手坐标系。
- `+Y` 向上，`+Z` 向前。
- CW 为正面；渲染使用 `CULL_CCW` 剔除背面。
- UV 原点为左上。
- 长度单位为米。

模型文件和它的全部动画文件必须使用同一个 source-coordinate preset。当前 Sour/Blender 预设是：右手、`+Z` 向上、`-Y` 向前、UV 左下、读取 FBX 文件单位元数据。坐标转换由导入器统一完成，运行时不得再为单个模型补旋转、镜像或缩放。

## 3. 模型 FBX

- 必须有至少一个可见蒙皮网格和一个目标骨架。
- 每顶点最多 4 个骨骼影响，权重和必须归一化为 1。
- 当前渲染骨骼调色板硬上限为 128；超过上限的角色必须在 DCC 中仅导出 deform bones，或等待引擎完成 per-submesh palette compaction 后再接入。
- 骨骼必须按父到子形成无环层级；名称非空且唯一。
- bind pose、inverse bind matrix 和蒙皮网格必须来自同一次模型导出。
- 正常可见的刚性附件应明确挂到骨骼；不要与 physics helper 混在同一命名层级。
- 不得导出 MMD rigid bodies、joints、collision shapes 或 `mmd_edge.*` 外扩描边壳作为普通可见网格。

### 材质与纹理

- 带纹理的模型 FBX 必须包含可被标准 FBX/Assimp 读取的 texture/file 引用；仅存在于 Blender/MMD 自定义节点组中的连接不算交付成功。
- 图片路径必须是相对路径，禁止依赖导出机器的绝对盘符。
- 推荐 Blender FBX `Path Mode = Copy`，并在交付前确认导出的 FBX 中仍能看到图片文件引用；是否 Embed Textures 可由项目决定。
- 材质名称必须稳定且唯一。重新导出不得改变材质与 submesh 的语义对应。
- 透明表面必须明确声明 Opaque、Mask 或 Blend；双面与 blend function 是独立属性。
- 法线贴图必须声明 `+Y`（OpenGL/Blender）或 `-Y`（DirectX）约定。

验收：一个声明使用纹理的角色如果导入后 `Texture=0`，或所有基础材质都没有 texture 参数，应直接判为源资产不合格，不能用白色默认材质掩盖。

## 4. 动画 FBX

- 动画 channel 的骨骼名称必须与模型目标骨架逐字节一致，包括大小写、命名空间和非 ASCII 字符。
- 不得在动画导出时重命名骨骼、改变父子拓扑或应用不同的轴/单位设置。
- 约束、IK、驱动器和控制器必须烘焙为 deform bones 的局部 TRS key；运行时不执行 DCC 约束图。
- 推荐一个文件一个逻辑 clip；多 take 文件按导出顺序生成多个 `.ayanm`。
- clip 时长必须大于 0，ticks-per-second 必须有效；建议 30 或 60 fps 烘焙。
- 动画文件可以包含用于导出的临时网格，但 `AnimationOnly` 会忽略它；正式交付仍建议关闭 mesh、material、rigidbody 和 edge 导出。
- 根运动策略必须在项目中统一：原地动画保持 root 水平位移为 0；root-motion 动画则只由约定的 root bone 携带位移。

## 5. 自动验收清单

导入器或 CI 至少检查：

1. 模型产出 `SkinnedMesh + Skeleton`，有材质的模型同时产出有效材质槽。
2. 声明带纹理的模型产出 Texture 资源和 Material→Texture 依赖。
3. 动画源在 `AnimationOnly` 下至少产出一个 Animation，且不产出 Mesh/Material/Texture。
4. 模型与动画使用完全相同的 source-coordinate tag。
5. 动画 track 名称全部能在目标 skeleton 中解析；未知 track 和未命中骨骼必须报告。
6. 目标骨架不超过当前 128-bone GPU palette 上限。
7. 每个 submesh 的材质槽有效，索引范围不越界，CW 正面契约通过。

不满足任一硬性条件时应中止发布导入；编辑器预览可 fail-soft 到 bind pose，但必须输出明确诊断。
