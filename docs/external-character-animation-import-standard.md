# AYEngine 外部角色与动画资产导入标准（v1）

**Version:** 1.1.0（文档版本）
**Date:** 2026-09-18
**Status:** Active — 现行交付契约；§6 为待实施管线设计
**Owner:** AYResource / AYAnimation / AYEditor

本文是 DCC、外包和资产商向 AYEngine 交付角色资产的最低契约。模型与动画文件承担不同职责；不要为了加入动画而重新导出并替换一个已经验证通过的模型文件。

标准参考骨架、尺寸、模型、纹理测试、版本及动画共享条件统一见 [AYHumanoid 标准骨架与参考模型规范](../../../AYDocs/AYHUMANOID-STANDARD.md)。本文负责外部 FBX 交付与导入操作；通用资产不强制使用参考模型的身高或完整 57 根骨。新增参考资产验收仍待实施。

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
- CCW 为正面；以 `AYFoundation/AYMath/include/AYMath/CoordinateConvention.h` 为准，渲染后端负责将该契约映射到剔除状态。
- UV 原点为左上。
- 长度单位为米。

默认使用 `Auto` source-coordinate 模式：每个 FBX 自己声明的 UpAxis、FrontAxis、CoordAxis 和单位元数据分别作为权威输入，再统一转换到引擎空间。模型和动画可以由不同 FBX 导出轴预设产生（例如模型 Z-up、动画 Y-up），但各文件的元数据必须真实、完整；运行时不得再为单个模型补旋转、镜像或缩放。`Manual` 仅用于已知元数据损坏且整批文件确实共享同一源坐标的资产，禁止把同一个手工 preset 强制用于轴声明不同的模型与动画。UV 原点仍由项目导入 preset 明确指定，当前 Blender 资产为左下。

模型最终可见尺寸必须在导出或离线导入阶段应用到 mesh bind space；运行时实体固定使用 `Scale=(1,1,1)`，不保留 `0.08`、`100` 等资产专用补偿值。对蒙皮模型，网格顶点与 skin cluster 的 inverse-bind matrix 是尺寸和 bind pose 的权威来源；Blender/FBX 为单位换算额外生成的 Object/Armature 包装缩放不得再次乘入顶点、骨架或 skin matrix。推荐在 Blender 导出前 Apply Scale；若导出器已把对象缩放应用到顶点，残留包装节点只作为 DCC 层级信息处理。骨骼节点自身用于变形的局部缩放仍会保留在动画中。

## 3. 模型 FBX

- 必须有至少一个可见蒙皮网格和一个目标骨架。
- 每顶点最多 4 个骨骼影响，权重和必须归一化为 1。
- 角色完整骨架没有 128 骨骼限制。导入 cook 会为每个渲染 submesh 建立局部骨骼调色板；若一个 submesh 使用的骨骼超过目标后端的单次绘制能力，则按三角形边界自动分块。
- 当前 uniform-buffer 后端的单次绘制调色板能力为 128；这是 cook profile / renderer capability，不是资产骨架上限。一个三角形本身使用的有效骨骼数不得超过该能力。
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

- 直接绑定模型的动画 channel 骨名必须与该模型的绑定骨架逐字节一致，包括大小写、命名空间和非 ASCII 字符。跨骨架动作则必须声明自己的源骨架并通过已验证 profile 重定向，不能把“名称必须一致”误解为所有外部来源都须先手工改名；这条后续管线尚待实现，当前直连要求不变。
- 不得在动画导出时重命名骨骼或改变父子拓扑；源轴/单位元数据必须准确，归一化后的绑定空间必须与模型一致。
- 动画与模型可以声明不同的 FBX 导出轴，但必须各自携带正确元数据，并在 `Auto` 模式下归一到相同引擎空间；不要用模型的手工轴 preset 覆盖动画。运行时不会保存动画文件的 Object/Armature 包装变换；根 deform bone 会先相对动画源的 skinned-mesh 节点求值，子 deform bone相对最近的 deform 父骨求值。因此 Blender/FBX 的轴、单位换算和 Armature 包装不会被重复旋转或缩放，真正的骨骼局部动画仍会保留。
- 约束、IK、驱动器和控制器必须烘焙为 deform bones 的局部 TRS key；运行时不执行 DCC 约束图。
- 推荐一个文件一个逻辑 clip；多 take 文件按导出顺序生成多个 `.ayanm`。
- clip 时长必须大于 0，ticks-per-second 必须有效；建议 30 或 60 fps 烘焙。
- 动画文件可以包含用于导出的临时网格，但 `AnimationOnly` 会忽略它；正式交付仍建议关闭 mesh、material、rigidbody 和 edge 导出。
- 当前 Assimp 动画层级烘焙需要从源文件识别 deform bones。动画 FBX 在导出时应保留可识别的 deform-bone/skin 元数据（可以携带仅用于识别的参考蒙皮网格，`AnimationOnly` 不会 cook 它）；仅含控制器 channel、完全没有骨骼分类信息的文件只能走名称直连兼容路径。
- 根运动策略必须在项目中统一：原地动画保持 root 水平位移为 0；root-motion 动画则只由约定的 root bone 携带位移。

## 5. 自动验收清单

导入器或 CI 至少检查：

1. 模型产出 `SkinnedMesh + Skeleton`，有材质的模型同时产出有效材质槽。
2. 声明带纹理的模型产出 Texture 资源和 Material→Texture 依赖。
3. 动画源在 `AnimationOnly` 下至少产出一个 Animation，且不产出 Mesh/Material/Texture。
4. 模型与动画均通过各自 FBX 元数据转换到同一个引擎坐标契约；若使用 `Manual`，必须确认两者源轴声明确实相同。
5. 直连动画的骨骼 track 名称全部能在绑定 skeleton 中解析；跨骨架路径则须在源骨架解析并验证映射/profile 和目标输出。未知骨骼 track 和未命中骨骼必须报告；非骨骼曲线按其类型验证，不按未知骨名删除。
6. cook 后每个 submesh 的局部骨骼调色板不超过目标 renderer capability，且所有局部索引都能映射到完整 skeleton。
7. 一个三角形的有效骨骼集合不得超过目标 renderer capability；否则应报告不可分割的源资产错误。
8. 每个 submesh 的材质槽有效，索引范围不越界，CCW 正面契约通过。
9. 运行时角色 Transform 为单位缩放；模型静止姿态对每根骨骼满足 `bindWorld * inverseBind == identity`，且动画根轨道不含 DCC 单位包装造成的额外倍率。

不满足任一硬性条件时应中止发布导入；编辑器预览可 fail-soft 到 bind pose，但必须输出明确诊断。

## 6. 编辑器原始骨架与发布烘焙（后续实现）

允许保留原始骨架，在导入阶段选择配置，或以后在编辑器手工映射/选择模板；保存配置不立即
改写源骨架。映射与重定向 profile 是可绑定骨架的一等作者资源，三个入口必须共享同一转换核心。
骨架资源图标和界面显示两个独立醒目标志：引擎适配、烘焙；映射有效不表示动画已经重定向，
已有导入缓存也不表示发布烘焙当前有效。

发布前必须生成清理后的派生 skeleton/mesh/clip，重写全部骨引用并验证构建摘要。清理不得删除
项目中的原始资源；有蒙皮、挂点、根运动、Mask、IK、物理等用途的扩展骨按策略保留，
不能只凭非 canonicalName 或零权重删除。过期或失败产物禁止打包，不回退到旧缓存。

这不是当前 importer/CookShip 已交付的功能。完整设计、资源边界、状态与 P0/P1/P2 实现项
统一见 [骨骼动画资源管线设计](../../../AYDocs/SKELETAL-ANIMATION-RESOURCE-PIPELINE.md)，不另行维护完成状态。
