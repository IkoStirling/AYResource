# AYResource

AYResource 是资产注册、导入、加载、缓存、热重载与 Cook/Ship 管线模块，维护资源 GUID、虚拟路径和运行时资产接口。

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
