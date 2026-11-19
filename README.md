# MHW:I Debug Hitbox, Scene Collision & Freeze Mod (15.23, DX11 + DX12 + HDR)

纯粹独立的实时碰撞体 / 攻击判定体（Hitbox）/ 受击判定体（Hurtbox）/ **地图交互实体与周围 `.sbc` 地形碰撞网格** / **隐藏怪物或地图模型材质（纯碰撞视图）** / **玩家无敌帧·霸体帧·GP·见切帧状态与剩余帧数**可视化及 **零延迟全局/世界冻结与逐帧步进** 插件（`debug_hitbox.dll`）。

> **职责拆分说明**：TAS 逐帧录制/回放、确定性 Checkpoint 与条件自动时停已完全独立拆分至平行项目 `mods\debug_tas`（产物 `nativePC\plugins\debug_tas.dll` / `out\debug_tas-15.23.zip`）。平时只需要看判定框、观察地形碰撞或时停练习时，仅安装 `debug_hitbox.dll` 即可。

## 安装方式（Stracker's Loader 插件）

1. 确保已安装 **Stracker's Loader**。
2. 将 `build\debug_hitbox.dll` 复制到游戏目录：
   ```text
   Monster Hunter World\nativePC\plugins\debug_hitbox.dll
   ```
   （如使用发布包 `out\debug_hitbox-15.23.zip`，直接解压到游戏根目录即可。）

## 默认热键一览

| 按键 | 功能 |
|---|---|
| **`U`** | **总开关**：一键开关实时碰撞体视图与场景隐藏/底色（关闭时 100% 恢复正常游戏画面，再次按 `U` 恢复上次筛选组合） |
| **`P`** | **打开 / 关闭交互式筛选与场景面板**（展开后用方向键/`Enter` 或数字键精确开关任意类别与材质隐藏，面板打开时自动拦截面板快捷键不误触游戏菜单；**打开聊天框 / 搜索框 / 起名输入文字时自动屏蔽全部插件快捷键**） |
| **`Shift + P`** | 不打开面板直接快速循环切换预设范围：`PL+EM COMBAT` → `PL+EM ONLY` → `MAP ONLY` → `HITBOX ONLY` → `HURTBOX ONLY` → `ALL (8/8)` |
| **`I`** | **全局冻结 / 恢复** — 0ms 零延迟暂停整个游戏逻辑（冻结期间仍可随时开关判定类别与隐藏怪物/地图模型） |
| **`K`** | **世界冻结（除玩家外全部暂停）** — 怪物、弹道/吐息特效、随从与机关定格，仅玩家与武器正常行动 |
| **`J`** | **单次触发判定框常驻开关（Keep Hitboxes Active）** — 默认开启（`[J] KEEP:ON`） |
| **`O`** | **逐帧步进（+1 帧）** — 在 `I` 或 `K` 冻结下精确推进 1 帧 |
| **`O / K / L / ;`** | **游戏视角旋转键自动禁用** — 只要 `debug_hitbox.dll` 存在，默认在引擎输入源头（`sMhKeyboard::updateState`）禁用 `O / K / L / ;` 调整镜头，确保按 `K`/`O` 永不转动相机（打开聊天框/起名框打字时自动放行，可通过 `BlockCameraKeys=0` 关闭） |

## `P` 交互式筛选与纯碰撞场景面板（13 项精细控制）

按下 **`P`** 展开左上角面板后，可用 **`↑` / `↓`** 选择行、**`←` / `→` / `Enter` / `Space`** 切换开关或调节参数，也可直接按行首快捷键秒切（按 **`P`** 或 **`ESC` / `Backspace`** 收起面板）：

| 面板内快捷键 | 筛选项 / 场景模式 | 说明 |
|---|---|---|
| `←` / `→`（第 0 行） | **SCOPE PRESET（预设范围）** | `PL+EM COMBAT`（默认：仅玩家+怪物攻受框，忽略地图）/ `PL+EM ONLY`（玩家+怪物+随从+推挤盒，完全忽略地图）/ `MAP ONLY`（**仅地图交互实体 + `.sbc` 地形网格，忽略玩家和怪物**）/ `HITBOX ONLY` / `HURTBOX ONLY` / `ALL (8/8)`；手动单改任意项自动变为 `CUSTOM` |
| **`=`** | **一键反转实体范围** | 在 **`PL+EM ONLY`（只看玩家和怪物，忽略地图）** 与 **`MAP ONLY`（只看地图实体与地形，忽略玩家和怪物）** 之间一键互切 |
| **`1`** | **Player Hurtbox + State** | 玩家受击框 + 无敌帧 / GP / 强霸体 / 见切反击帧染色与剩余帧数 |
| **`2`** | **Player / Weapon Hitbox** | 玩家近战武器、弹药、猎虫、钩爪攻击判定框（亮金色） |
| **`3`** | **Palico / Companion** | 随从猫 / 兽人族受击框（天蓝色）与攻击框（琥珀橙） |
| **`4`** | **Monster Hurtboxes** | 怪物全身部位受击肉质框（翠绿色） |
| **`5`** | **Monster / Shell Hitbox** | 怪物近战、吐息、激光、飞弹与场地危害攻击判定框（亮红色） |
| **`6`** | **Body Pushboxes** | 玩家与怪物物理防穿模推挤盒（洋红紫） |
| **`7`** | **Map Entities / Gimmicks** | 地图交互实体碰撞框：训练场木桩/木车、爆桶、落石、藤蔓陷阱、麻痹蛙/环境生物等（受击框薄荷青绿 / 爆炸与落石攻击框珊瑚橙） |
| **`8`** | **Map Terrain Mesh (.sbc)** | 周围 `.sbc` 地形轻盈半透明碰撞面 + 折角棱线（默认大幅降低半透明面不透明度至约 `1/3` 避免遮挡视线，保留清晰台阶/墙角棱线与 `2m` 等高线带；选中第 8 行按 `←`/`→` 可切换半径 **`15m` / `25m` / `45m` / `80m`**） |
| **`9`** | **Hide Monster Mesh** | **引擎级隐藏怪物模型与材质**（同时隐藏怪物本体、破坏部位与怪物阴影，100% 保留怪物 AI、骨骼更新与碰撞体，**玩家 UI 100% 正常显示**） |
| **`0`** | **Hide Map / Stage Mesh** | 三档切换：`OFF` → **`KEEP INTERACT`（隐藏静态地面/墙壁/植被/天空水体，保留楔虫、采集点、营地帐篷/道具箱、机关与环境生物可见且 100% 可交互）** → **`HIDE ALL MAP`（连同地图交互实体与 `*InstanceTakeOver` 合批外观一并隐藏，同时为直接单元与合批子单元同步置位 `+0x54` 延迟/前向通道掩码，确保所有隐藏元素 100% 可正常交互，且玩家 UI 100% 正常显示！）** |
| **`-`** | **Pure 3D / Canvas** | **纯碰撞 3D 隐藏与画布模式（全程保留玩家 UI 100% 原生亮度与显示）**：`OFF` → **`HIDE PL/VFX`（隐藏玩家、武器、随从猫与 3D 特效模型）** → **`HIDE ALL 3D`（隐藏全部 3D 场景 + 机关 + 怪物 + 玩家 + 特效网格，获得纯黑底纯碰撞视图且玩家 UI 100% 清晰可见、全部地图交互正常可用）** → **`DIM 45%`（通过 3D 视口 `uColorCorrectFilter` 仅在 2D 玩家 UI 合成前将 3D 场景柔暗 45%，玩家血条/斩味/小地图/道具栏保持 100% 原生满亮度！）** |
| **`/`** | **Terrain Style & Rings** | **地形不透明/半透明立体风格与地面距离标尺模式（共 6 档，全模式支持 3D 角色/怪物/机关深度遮挡与 2D 玩家 UI 置顶保护）**：`SEMI 3D+RINGS`（默认：半透明 3D `.sbc` 地形 + 地面距离标尺环，自动被前景角色/怪物/机关遮挡且不遮挡玩家 UI）→ **`OPAQUE 3D+RING`（对齐引擎 `0x1423D0E70` 硬件 Reversed-Z 投影的 100% 正向不透明实体 `.sbc` 地形 + 前景角色/怪物/可交互元素正确遮挡 + 原生色彩空间还原 2D 玩家 UI 与 `"F"` 交互提示 + 地面标尺环）** → **`OPAQUE 3D ONLY`（100% 正向不透明实体 `.sbc` 地形，不画地面环）** → `SEMI+RINGS ON`（半透明 3D 地形 + 常驻距离环）→ `SEMI 3D ONLY`（仅半透明 3D 地形）→ `RAW WIREFRAME`（原始全三角方格线框） |

## 可选配置文件 `nativePC\plugins\debug_hitbox.ini`

```ini
Enabled=1
KeyToggleVis=U
KeyToggleFreeze=I
KeyFreezeOthers=K
KeyPersistHitbox=J
KeyStepFrame=O
KeyCycleFilter=P

StartVisOn=0
KeepHitboxesActive=1
; 插件加载期间禁用 O / K / L / ; 旋转游戏视角（聊天输入时自动放行）
BlockCameraKeys=1
ShowHudBanner=1
LineThickness=2.0
OpacityScale=1.0
; .sbc 地形半透明面额外透明度倍率（默认 1.0，范围 0.1 ~ 3.0）
SbcSurfaceOpacity=1.0
HdrNits=0

; 默认启用类别掩码（位掩码：0x1B=PL+EM COMBAT, 0x3F=PL+EM ONLY, 0xC0=MAP ONLY, 0xFF=ALL）
CategoryMask=0x1B
; 地形网格默认显示半径（米：15 / 25 / 45 / 80）
SbcRadiusMeters=25
; 开启 U 时默认隐藏模式：StartHideStage (0=OFF, 1=KEEP INTERACT, 2=HIDE ALL MAP)
StartHideMonster=0
StartHideStage=0
; 纯碰撞 3D 隐藏 / 画布（0=OFF, 1=HIDE PL/VFX, 2=HIDE ALL 3D, 3=DIM 45%[仅压暗3D不压暗UI]）
StartBackdrop=0
; 地形风格与标尺（0=SEMI 3D+RINGS, 1=OPAQUE 3D+RING, 2=OPAQUE 3D ONLY, 3=SEMI+RINGS ON, 4=SEMI 3D ONLY, 5=RAW WIRE）
DepthGuideMode=0
```

## 待机完全原生零开销架构（彻底消除 1% Low 掉帧）

1. **0 后台轮询线程、0 每帧 `win32k` 系统调用**：初始化完成后不保留任何后台线程；通过进程内窗口线程专属 `WH_KEYBOARD` 事件钩子响应热键（面板关闭时 `0` 按键拦截；面板打开时精准吞掉导航键防止误触菜单），彻底消除每帧 `GetAsyncKeyState` / `GetForegroundWindow` 在高回报率鼠标下引发的 `win32k` 内核锁竞争。
2. **正常游玩时 `.text` 模拟与场景渲染函数 100% 原生机器码（0 Hook）**：启动时**仅**挂载 `cD3DRender::present`（每帧 1 次）；`cUnit::genericMove`（`0x142228F50`）、`HitDispatch`（`0x1412DD7C0`）与场景绘制 `sUnit::draw`（`0x14221A050`）在未开启对应功能时**完全不打补丁、保持 100% 原版字节**，仅在冻结或开启模型隐藏期间通过 8B 对齐硬件原子指令 `LOCK CMPXCHG8B` 按需挂载，关闭后即刻原子还原原版机器码。
3. **`Present` 待机 1 字节无栈帧尾跳转**：未开启显示且未冻结时，`Hook_D3DPresent` 仅执行单条 `cmp byte ptr [g_presentWorkMask], 0` 后直接尾跳转回原函数（每帧仅约 3 纳秒，0 锁、0 D3D 调用、0 游戏内存读取）。

## 编译方式

```batch
mods\debug_hitbox\build.bat
```

产物：`mods\debug_hitbox\build\debug_hitbox.dll`（发布包：`out\debug_hitbox-15.23.zip`，仅含 `nativePC/plugins/debug_hitbox.dll`）。
