# MHW Hitbox & Frame Inspector (15.23)

[中文说明](#中文说明) | [English Guide](#english-guide)

---

## 中文说明

面向《怪物猎人：世界 冰原》（Steam 15.23）的实时判定框可视化、怪物部位肉质/软化分区检视、地图地形碰撞查看与零延迟时停步进工具，兼容 DirectX 11 / DirectX 12、16:9 / 21:9 超宽屏以及 SDR / HDR 显示。

### 前置要求与安装

1. 游戏版本：**Monster Hunter: World Iceborne 15.23**（Steam）。
2. 前置插件：**Stracker's Loader**。
3. 将压缩包内的 `nativePC` 文件夹解压至游戏根目录，确认最终路径为：
   ```text
   <游戏根目录>\nativePC\plugins\mhw_hfi.dll
   <游戏根目录>\nativePC\plugins\mhw_hfi.ini
   ```

---

### 功能总览

#### 1. 实时攻受判定框与推挤盒可视化
支持 8 个独立类别按需开关或通过预设一键切换：
- **玩家受击框 + 防御帧状态**：按当前动作实时染色并提示剩余帧数/秒数，区分无敌帧（翻滚/垫步/招式无敌）、GP 防御判定、普通防御、强霸体（含不动衣装）、见切/防反窗、普通霸体。
- **玩家攻击判定框**：近战武器、弹药、猎虫与钩爪攻击框（亮金色）。
- **随从猫 / 兽人族**：独立受击框（天蓝色）与攻击框（琥珀橙）。
- **怪物受击框（Hurtbox）**：全身各部位受击判定体（默认翠绿色，可在怪物部位检视器中分部位染色与过滤）。
- **怪物攻击判定框（Hitbox）**：近战、吐息、激光、飞弹及场地危害判定体（亮红色）。
- **物理推挤盒（Pushbox）**：玩家与怪物防穿模体积碰撞盒（洋红紫）。
- **地图交互实体**：训练场木桩/木车、爆桶、落石、藤蔓陷阱、麻痹蛙/环境生物等（薄荷青绿受击框 / 珊瑚橙攻击框）。
- **地图地形碰撞网格**：角色周围 `15m / 25m / 45m / 80m` 范围内的静态与动态地形碰撞体，支持 6 种立体/实体/纯线框显示风格、低模/高模碰撞层级独立查看、非实体特殊碰撞层（水面/换区空气帘/镜头挡板等）6 档透明度调节，以及实体线宽与地形线宽独立缩放。

#### 2. 怪物部位分区检视器（实时肉质 / 软化 / 猎虫精华）
专为深度研究特定怪物打造，可同时联动过滤受击框与对应部位的物理推挤盒：
- **细粒度肉质分区（`HITZONE`）**：按怪物全身独立解剖部位分别着色，实时显示：
  - 分色猎虫精华 `[R/W/O/G]`（红/白/橙/绿）
  - 实时物理肉质 `S/B/A`（斩/打/弹，随形态变化、部位破坏及钩爪软化实时更新，软化中带 `*` 与暖金高亮）
  - 分色实时属性吸收 `F/W/T/I/D`（火/水/雷/冰/龙）
  - 部位包含的受击框与推挤盒数量 `(nH+mP)`，并保留独立的中心辅助球槽位（`AUX CTR`）
- **钩爪软化分区（`CLUTCH`）**：按怪物可独立软化的钩爪区域划分并独立着色；不可钩爪软化的部位统一收束为独立的 `NO-CLAW` 槽位，方便单独隐藏或查看。
- **精细筛选与巡检**：支持多选任意部位、一键独显单个部位（`SOLO`）与逐部位步进浏览、未选中部位选择完全隐藏（`HIDE`）或极淡轮廓参照（`GHOST OUTLINE`）、多怪同场目标锁定，以及 4 档 3D 悬浮标签显示模式。

#### 3. 场景模型隐藏与纯碰撞视图
全部模式均完整保留游戏内玩家 UI 与地图正常交互能力：
- **隐藏怪物模型**：隐藏怪物外观与阴影，仅保留判定框观察。
- **隐藏地图模型**：支持 `KEEP INTERACT`（隐藏静态地形/植被，保留采集点/楔虫/营地箱子/机关可见）与 `HIDE ALL MAP`（全隐藏且保持交互有效）。
- **纯碰撞 3D / 画布模式**：支持隐藏玩家与特效（`HIDE PL/VFX`）、全 3D 模型隐藏获得纯黑底碰撞画布（`HIDE ALL 3D`），以及仅柔暗 3D 背景而保持 UI 满亮度的 `DIM 45%`。

#### 4. 零延迟冻结、世界冻结与单帧步进
- **全局冻结（Full Freeze）**：瞬时零延迟定格整个游戏，屏幕顶部居中与四角显示显眼冻结提示（即使关闭 `[U]` 视图也始终可见），冻结期间可自由调整面板筛选与观察细节。
- **世界冻结（World Freeze）**：仅暂停怪物、弹道/吐息特效、随从与机关，玩家与武器照常自由行动，重置/切换任务或返回据点时自动解除冻结，便于安全测试招式判定、打点、翻滚无敌帧与 GP/见切。
- **单次判定框常驻（Keep Hitboxes Active）**：世界冻结下保持火球、吐息、单次近战/投技等判定框在命中或防御后不消失，可反复练习翻滚、见切与 GP。
- **世界冻结延迟爆发（World Freeze Delayed Burst，默认开启）**：修复世界冻结下连续攻击导致的去重缓存（`cCache[32]`）与固定特效/伤害池溢出崩溃；冻结期间照常保留实时命中特效、武器命中音效、卡肉与运镜震动并暂存伤害数字，解除时停后按节奏错峰爆发重放伤害数字、命中特效、优先级运镜与视野冲击（FOV Punch），可通过 `[P]` 面板第 18 行或聊天命令 `$freeze_burst` 实时切换。
- **逐帧步进（+1 Frame）**：在全局冻结或世界冻结下精确推进 1 帧。

#### 5. 游戏内聊天 / 快捷轮盘定型文触发与手柄操控面板
- **聊天 / 轮盘定型文激活**：通过游戏内聊天或快捷轮盘定型文触发 8 个核心功能，绕开手柄按键紧张的限制；仅响应本地玩家主动发言，支持连续重复发送同一条定型文，且支持在游戏运行中修改 `mhw_hfi.ini` 后免重启热刷新。
- **手柄操控面板**：打开筛选面板后，可直接使用手柄十字键与 `A`（PS `×`）键浏览和修改所有选项，面板展开期间自动避免误触翻滚与道具栏滚动，按住 `LB` 呼出快捷轮盘或打开聊天框打字时自动放行。

---

### 操作方式

#### 1. 键盘主热键（默认）

| 按键 | 功能 |
|---|---|
| **`U`** | 总开关：开启 / 关闭判定框与场景视图 |
| **`I`** | 全局冻结 / 恢复（Full Freeze） |
| **`K`** | 世界冻结 / 恢复（仅玩家可动，World Freeze） |
| **`J`** | 开关单次触发判定框常驻（Keep Hitboxes Active，默认开启） |
| **`O`** | 单帧步进 +1 帧（在 `I` 或 `K` 冻结状态下生效） |
| **`P`** | 打开 / 关闭左上角交互式筛选与场景面板 |
| **`Shift + P`** | 不打开面板直接循环切换预设范围 |
| **`\`** | 显示 / 隐藏插件 2D Overlay（顶部状态条、冻结横幅、四角框线并关闭面板），便于纯净截图 |

> - 打开游戏内聊天框、搜索框或起名输入文字时，插件会自动暂停拦截全部热键。
> - 插件启用期间默认屏蔽游戏原生 `O / K / L / ;` 键盘转视角键（打字时自动放行），避免按 `K` / `O` 时误转镜头；可在 `mhw_hfi.ini` 中将 `BlockCameraKeys` 设为 `0` 恢复。

#### 2. 交互式 `[P]` 面板操作（主面板）

按 **`P`** 展开面板后：
- **`↑` / `↓`**：上下移动选中行（支持长按连发）
- **`←` / `→`**：向左 / 向右调节当前行选项或数值
- **`Space` / `Enter`**：切换当前行开关或步进模式（在第 17 行可进入怪物部位检视器）
- **`TAB`**：在“主面板”与“怪物部位检视器”两页间切换
- **`P` / `ESC` / `Backspace`**：关闭面板
- **单行快捷热键（仅键盘）**：

| 快捷键 | 对应行 | 功能说明 |
|---|---|---|
| `←` / `→`（第 0 行） | Scope Preset | 循环切换 `PL+EM COMBAT` / `PL+EM ONLY` / `MAP ONLY` / `HITBOX ONLY` / `HURTBOX ONLY` / `ALL (8/8)` |
| **`=`** | 反转实体范围 | 在 `PL+EM ONLY` 与 `MAP ONLY` 之间一键互切 |
| **`1`** | Player Hurtbox | 开关玩家受击框与防御帧状态 |
| **`2`** | Player Hitbox | 开关玩家武器/弹药/猎虫/钩爪攻击框 |
| **`3`** | Palico / Otomo | 开关随从猫攻受框 |
| **`4`** | Monster Hurtbox | 开关怪物受击框 |
| **`5`** | Monster Hitbox | 开关怪物近战/吐息/弹体攻击框 |
| **`6`** | Body Pushbox | 开关玩家与怪物物理推挤盒 |
| **`7`** | Map Entities | 开关地图交互实体碰撞框 |
| **`8`** | Map Terrain | 开关地形碰撞网格（选中该行按 `←` / `→` 切换 `15m / 25m / 45m / 80m` 半径） |
| **`9`** | Hide Monster Mesh | 开关隐藏怪物模型 |
| **`0`** | Hide Map / Stage Mesh | 循环切换 `OFF` / `KEEP INTERACT` / `HIDE ALL MAP` |
| **`-`** | Pure 3D / Canvas | 循环切换 `OFF` / `HIDE PL/VFX` / `HIDE ALL 3D` / `DIM 45%` |
| **`/`** | Terrain Style & Rings | 循环切换 6 种地形风格（半透明 3D、不透明实体 3D、地面距离环、纯线框） |
| **`,`** | SBC Collision Tier | 循环切换 `LOW ONLY` / `LOW + HIGH` / `HIGH ONLY` |
| **`.`** | Special SBC Layers | 循环切换非实体特殊层透明度：`OFF` / `GHOST` / `LIGHT` / `MED` / `VIVID` / `OPAQUE` |
| `←` / `→`（第 15 行） | Entity Line Width | 调节怪物/玩家/机关线条粗细（`0.35x` ~ `3.00x`） |
| `←` / `→`（第 16 行） | SBC / Raw Line Width | 调节地形网格/纯线框/距离环线条粗细（`0.35x` ~ `3.00x`） |
| **`TAB`**（或第 17 行） | Monster Part Filter | 选中按 `←` / `→` 快切分区模式，按 `Space` / `Enter` 进入怪物部位检视器 |
| `←` / `→`（第 18 行） | Freeze Delayed Burst | 开关世界冻结延迟爆发模式（`ON` / `OFF`，默认 `ON`） |

#### 3. `[P]` → `[TAB]` 怪物部位检视器操作

- **顶部控制行（第 0~4 行）**：
  - `PARTITION MODE`：切换 `OFF` / `HITZONE (FINE)` / `CLUTCH (SOFTEN)`
  - `UNSELECTED PARTS`：切换未选中部位 `HIDE` / `GHOST OUTLINE`
  - `3D PART LABELS`：切换 3D 悬浮标签 `PHYS + ELEM + EXT` / `PHYS + EXTRACT` / `ID + EXTRACT` / `OFF`
  - `TARGET MONSTER`：切换 `AUTO (SOLO)` / `AUTO (+OTHERS)` / 指定锁定 `#1..#8`
  - `QUICK PART SELECT`：按 `Space` / `Enter` 在全部开启与独显间切换，按 `←` / `→` 逐个部位顺序独显巡检
- **部位列表行**：按 `Space` / `Enter` / `←` / `→` 开关当前部位；键盘可按 `1`~`0`、`-`、`/`、`,`、`.` 秒切前 14 个部位，按 **`=`** 一键独显/取消独显当前高亮部位。
- **返回主面板**：按 **`TAB`**，或移动光标至末行 **`<<< BACK TO MAIN PANEL`**（在第 0 行按一次 `↑` 即可直接回绕选中）后按 `Space` / `Enter` / `A`。

#### 4. 手柄操控 `[P]` 面板

当 `[P]` 面板打开时，可直接用手柄操作（键盘单行数字/符号快捷键不适用于手柄）：

| 手柄按键 | 等效操作 |
|---|---|
| **十字键 `↑` / `↓`** | 上下移动光标（长按自动连发） |
| **十字键 `←` / `→`** | 调节当前行参数或开关 |
| **`A` 键（Xbox `A` / PS `×`）** | 等同键盘 `Space`（切换开关、步进选项、进出怪物部位子面板） |
| **按住 `LB`** | 临时放行手柄输入，便于在面板展开时仍正常用快捷轮盘切页并发送定型文 |

#### 5. 游戏内聊天 / 快捷轮盘定型文触发

支持触发 7 个核心主热键及世界冻结延迟爆发开关，采用**完整字符串精确匹配**（可在游戏内直接打字发送，或填入自定义定型文并绑到快捷轮盘）：

| 对应主热键 | 功能 | 默认触发字符串 | `.ini` 配置项 |
|---|---|---|---|
| **`U`** | 开关判定框与场景视图 | `$draw_hitbox` | `ChatCmdVis` |
| **`I`** | 全局冻结 / 恢复 | `$full_freeze` | `ChatCmdFreeze` |
| **`K`** | 世界冻结 / 恢复 | `$ThE_WoRLD!!!` | `ChatCmdWorldFreeze` |
| **`O`** | 单帧步进 +1 帧 | `$step_frame` | `ChatCmdStep` |
| **`J`** | 开关单次判定框常驻 | `$keep_hitbox` | `ChatCmdKeepHitbox` |
| **`P`** | 打开 / 关闭筛选面板 | `$show_panel` | `ChatCmdPanel` |
| **`\`** | 显示 / 隐藏插件 2D Overlay（纯净截图） | `$show_overlay` | `ChatCmdOverlay` |
| `—`（`[P]` 第 18 行） | 开关世界冻结延迟爆发 | `$freeze_burst` | `ChatCmdFreezeBurst` |

- **默认不区分英文大小写**（`ChatCaseSensitive=0`），可在 `mhw_hfi.ini` 中改为 `1` 开启严格区分大小写。
- **自定义与热刷新**：游戏运行期间随时编辑并保存 `nativePC\plugins\mhw_hfi.ini`（中文/日文定型文请保存为 UTF-8 编码），下一次发聊天/定型文或打开面板时即时生效，无需重启游戏。将任意一项留空或填 `NONE` 可单独禁用该命令。

---

### 配置文件（`nativePC\plugins\mhw_hfi.ini`）

压缩包中已附带包含全部默认值的 `mhw_hfi.ini`。所有配置项均可选，缺省时使用默认值：

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `Enabled` | `1` | 插件总开关（`1` 启用 / `0` 禁用） |
| `KeyToggleVis` / `KeyToggleFreeze` / `KeyFreezeOthers` / `KeyPersistHitbox` / `KeyStepFrame` / `KeyCycleFilter` / `KeyToggleOverlay` | `U` / `I` / `K` / `J` / `O` / `P` / `\` | 自定义 7 个主热键（支持字母、数字、`F1`~`F24`、`NUMPAD0`~`9`、`\` 等） |
| `StartVisOn` | `0` | 启动游戏后默认是否自动开启视图 |
| `KeepHitboxesActive` | `1` | 世界冻结下默认开启单次判定框常驻 |
| `WorldFreezeDelayedBurst` | `1` | 世界冻结延迟爆发模式：防止连击溢出崩溃，时停保留实时特效/音效/卡肉/震动，解除后错峰爆发伤害数字、特效、运镜与视野冲击 |
| `BlockCameraKeys` | `1` | 屏蔽游戏 `O / K / L / ;` 转视角键 |
| `ShowHudBanner` | `1` | 显示左上角状态提示条 |
| `ChatCommands` | `1` | 开关聊天/定型文触发（支持热刷新） |
| `ChatCaseSensitive` | `0` | 聊天命令是否区分英文大小写（支持热刷新） |
| `ChatCmdVis` / `ChatCmdFreeze` / `ChatCmdWorldFreeze` / `ChatCmdStep` / `ChatCmdKeepHitbox` / `ChatCmdPanel` / `ChatCmdOverlay` / `ChatCmdFreezeBurst` | `$draw_hitbox` / `$full_freeze` / `$ThE_WoRLD!!!` / `$step_frame` / `$keep_hitbox` / `$show_panel` / `$show_overlay` / `$freeze_burst` | 自定义 8 个聊天/定型文触发词（UTF-8，支持热刷新） |
| `GamepadPanel` | `1` | 开关 `[P]` 面板手柄操控（支持热刷新） |
| `LineThickness` / `EntityLineScale` / `SbcLineScale` | `2.0` / `1.0` / `1.0` | 基础线宽与实体/地形线宽倍率 |
| `OpacityScale` / `SbcSurfaceOpacity` / `HdrNits` | `1.0` / `1.0` / `0` | 全局透明度、地形半透明面透明度、HDR 亮度（`0` 为自动） |
| `CategoryMask` / `SbcRadiusMeters` | `0x1B` / `25` | 初始显示类别掩码与地形显示半径 |
| `StartHideMonster` / `StartHideStage` / `StartBackdrop` | `0` / `0` / `0` | 初始模型隐藏与纯碰撞画布模式 |
| `DepthGuideMode` / `SbcMeshTier` / `SpecialSbcStyle` | `0` / `1` / `2` | 初始地形风格、碰撞层级与特殊层样式 |
| `EmPartMode` / `EmUnselectedStyle` / `EmPartLabels` | `0` / `0` / `0` | 初始怪物部位分区、未选中样式与 3D 标签模式 |

---

## English Guide

A real-time hitbox/hurtbox visualization, monster part hitzone & Clutch Claw soften-zone inspector, stage terrain collision viewer, and zero-delay freeze/frame-stepping tool for **Monster Hunter: World Iceborne (Steam 15.23)**, compatible with DirectX 11 / DirectX 12, 16:9 / 21:9 ultrawide displays, and SDR / HDR.

### Requirements & Installation

1. Game version: **Monster Hunter: World Iceborne 15.23** (Steam).
2. Prerequisite plugin: **Stracker's Loader**.
3. Extract the `nativePC` folder from the archive into your game root directory and verify the final paths:
   ```text
   <MHW Root>\nativePC\plugins\mhw_hfi.dll
   <MHW Root>\nativePC\plugins\mhw_hfi.ini
   ```

---

### Feature Overview

#### 1. Real-Time Hitbox, Hurtbox & Pushbox Visualization
Supports 8 independent categories that can be toggled individually or switched in one step via presets:
- **Player Hurtbox + Defensive Frame State**: Color-codes the player in real time based on the current action and displays remaining frames/seconds, distinguishing Invincibility frames (rolls/hops/action i-frames), Guard Point, normal Guard, Hyper Armor (including Rocksteady Mantle), Counter / Foresight windows, and Super Armor.
- **Player Hitboxes**: Melee weapons, ammo/shells, Kinsect, and Clutch Claw attack volumes (bright gold).
- **Palico / Companions**: Separate hurtboxes (sky blue) and hitboxes (amber orange).
- **Monster Hurtboxes**: Full-body hurtboxes across all monster parts (emerald green by default; can be color-coded and filtered per part in the Monster Parts Inspector).
- **Monster Hitboxes**: Melee swings, breaths, beams, projectiles, and stage hazards (bright red).
- **Body Pushboxes**: Physical anti-clipping collision volumes for players and monsters (magenta).
- **Map Interactive Entities**: Training pole/cart, barrel bombs, falling boulders, vine traps, paratoads/endemic life, etc. (mint hurtboxes / coral orange hitboxes).
- **Map Terrain Collision Mesh**: Static and dynamic terrain collision geometry within `15m / 25m / 45m / 80m` of the player, with 6 visual styles (3D translucent, solid opaque, raw wireframe), separate Low/High collision tier viewing, 6 opacity steps for special non-solid layers (water, zone boundaries, camera blockers, etc.), and independent line width scaling for entities and terrain.

#### 2. Monster Parts Inspector (Live Hitzones / Tenderize / Kinsect Extract)
Built for in-depth study of specific monsters, filtering hurtboxes and their matching physical body pushboxes together:
- **Fine Hitzone Partition (`HITZONE`)**: Color-codes each anatomical part of the monster independently and displays in real time:
  - Color-coded Kinsect Extract `[R/W/O/G]` (Red / White / Orange / Green)
  - Live Physical Hitzones `S/B/A` (Sever / Blunt / Shot, updating live on state changes, part breaks, and Clutch Claw tenderizing, marked with `*` and warm-gold highlight while tenderized)
  - Color-coded Live Elemental Hitzones `F/W/T/I/D` (Fire / Water / Thunder / Ice / Dragon)
  - Hurtbox and pushbox count per part `(nH+mP)`, while keeping an independent slot for the center helper sphere (`AUX CTR`)
- **Clutch Claw Soften Zone Partition (`CLUTCH`)**: Partitions and independently color-codes the monster by tenderizable Clutch Claw zones; parts that cannot be clutched/tenderized are consolidated into a dedicated `NO-CLAW` slot so they can be hidden or inspected separately.
- **Fine-Grained Filtering & Inspection**: Supports multi-selecting any combination of parts, one-key solo isolation (`SOLO`) and sequential part-by-part stepping, choosing between completely hiding unselected parts (`HIDE`) or showing faint posture outlines (`GHOST OUTLINE`), multi-monster target locking, and 4 modes of 3D floating part labels.

#### 3. Scene Model Hiding & Pure Collision View
All modes preserve the in-game player UI and normal map interactions:
- **Hide Monster Mesh**: Hides monster models and shadows so you can focus purely on collision volumes.
- **Hide Map / Stage Mesh**: Supports `KEEP INTERACT` (hides static terrain/foliage while keeping gathering points, wedge beetles, camp boxes, and gimmicks visible) and `HIDE ALL MAP` (hides all map visuals while keeping interactions functional).
- **Pure 3D / Canvas Mode**: Supports hiding player and VFX (`HIDE PL/VFX`), hiding all 3D models for a pure black collision canvas (`HIDE ALL 3D`), and `DIM 45%` which gently dims only the 3D background while keeping the UI at full brightness.

#### 4. Zero-Delay Freeze, World Freeze & Single-Frame Stepping
- **Full Freeze**: Instantly freezes the entire game with zero delay, displaying a prominent top-center banner and corner brackets (always visible even when `[U]` visualization is OFF); you can still adjust panel filters and inspect details while frozen.
- **World Freeze**: Pauses only monsters, projectiles/breath effects, companions, and gimmicks while the player and weapon move freely, and automatically unfreezes on quest reset, stage transitions, or returning to Hub—ideal for safely testing attack hitboxes, spacing, roll i-frames, Guard Points, and Foresight/counters.
- **Keep Hitboxes Active**: Keeps single-trigger hitboxes (fireballs, breaths, single-hit swings/grabs) from disappearing after hitting or being guarded during World Freeze, allowing repeated dodge, counter, and GP practice.
- **World Freeze Delayed Burst (ON by default)**: Fixes dedup cache (`cCache[32]`) and fixed-size VFX/damage pool overflows caused by repeated hits during World Freeze; keeps hit VFX, weapon hit sounds, hitstop, and camera shakes live during freeze while holding back damage numbers, then bursts out paced damage numbers, hit VFX, priority-ranked camera shakes, and a resume FOV punch when World Freeze ends. Toggle live via `[P]` panel row 18 or `$freeze_burst`.
- **Single-Frame Step (+1 Frame)**: Advances exactly 1 frame while in Full Freeze or World Freeze.

#### 5. In-Game Chat / Radial Shoutout Triggers & Controller Panel Control
- **Chat / Radial Shoutout Activation**: Trigger the 8 core functions via in-game chat or Radial Menu custom shoutouts, bypassing tight controller button budgets; responds only to local player messages, supports sending the same shoutout consecutively to trigger repeatedly, and supports hot-reloading edits to `mhw_hfi.ini` while the game is running without restarting.
- **Controller Panel Navigation**: Once the filter panel is open, browse and adjust all options directly with the controller D-Pad and `A` (PS `×`). While the panel is open, accidental rolls and item bar scrolling are prevented automatically; holding `LB` for the Radial Menu or opening a chat/text input automatically passes inputs through to the game.

---

### Controls & Operation

#### 1. Main Keyboard Hotkeys (Default)

| Key | Action |
|---|---|
| **`U`** | Master toggle: turn collision & scene visualization ON / OFF |
| **`I`** | Full Freeze / Resume |
| **`K`** | World Freeze / Resume (only the player can move) |
| **`J`** | Toggle Keep Single-Trigger Hitboxes Active (default ON) |
| **`O`** | Step +1 frame (active while frozen by `I` or `K`) |
| **`P`** | Open / close the top-left interactive Filter & Scene Panel |
| **`Shift + P`** | Cycle scope presets directly without opening the panel |
| **`\`** | Show / hide plugin 2D Overlay (top HUD banner, freeze banner, corner brackets & closes panel) for clean screenshots |

> - All mod hotkeys automatically pause while typing in in-game chat, search, or naming boxes.
> - While the mod is active, native keyboard camera rotation keys `O / K / L / ;` are blocked by default (passed through automatically when typing) so pressing `K` or `O` never nudges the camera; set `BlockCameraKeys=0` in `mhw_hfi.ini` to restore them.

#### 2. Interactive `[P]` Panel Operation (Main Panel)

After opening the panel with **`P`**:
- **`Up` / `Down`**: Move cursor up/down (supports hold auto-repeat)
- **`Left` / `Right`**: Adjust the selected row's option or value left/right
- **`Space` / `Enter`**: Toggle the selected row or step its mode (on row 17, opens the Monster Parts Inspector)
- **`TAB`**: Switch between the Main Panel and the Monster Parts Inspector
- **`P` / `ESC` / `Backspace`**: Close the panel
- **Direct Single-Row Shortcuts (Keyboard Only)**:

| Shortcut | Row | Description |
|---|---|---|
| `Left` / `Right` (Row 0) | Scope Preset | Cycle `PL+EM COMBAT` / `PL+EM ONLY` / `MAP ONLY` / `HITBOX ONLY` / `HURTBOX ONLY` / `ALL (8/8)` |
| **`=`** | Flip Entity Scope | One-key toggle between `PL+EM ONLY` and `MAP ONLY` |
| **`1`** | Player Hurtbox | Toggle player hurtbox and defensive frame state |
| **`2`** | Player Hitbox | Toggle player weapon/ammo/Kinsect/Clutch Claw hitboxes |
| **`3`** | Palico / Otomo | Toggle Palico hurtboxes & hitboxes |
| **`4`** | Monster Hurtbox | Toggle monster hurtboxes |
| **`5`** | Monster Hitbox | Toggle monster melee/breath/projectile hitboxes |
| **`6`** | Body Pushbox | Toggle player & monster physical pushboxes |
| **`7`** | Map Entities | Toggle map interactive entity collision volumes |
| **`8`** | Map Terrain | Toggle terrain collision mesh (select this row and press `Left` / `Right` to switch radius `15m / 25m / 45m / 80m`) |
| **`9`** | Hide Monster Mesh | Toggle hiding monster models |
| **`0`** | Hide Map / Stage Mesh | Cycle `OFF` / `KEEP INTERACT` / `HIDE ALL MAP` |
| **`-`** | Pure 3D / Canvas | Cycle `OFF` / `HIDE PL/VFX` / `HIDE ALL 3D` / `DIM 45%` |
| **`/`** | Terrain Style & Rings | Cycle 6 terrain styles (semi-transparent 3D, opaque solid 3D, ground distance rings, raw wireframe) |
| **`,`** | SBC Collision Tier | Cycle `LOW ONLY` / `LOW + HIGH` / `HIGH ONLY` |
| **`.`** | Special SBC Layers | Cycle special non-solid layer opacity: `OFF` / `GHOST` / `LIGHT` / `MED` / `VIVID` / `OPAQUE` |
| `Left` / `Right` (Row 15) | Entity Line Width | Adjust monster/player/entity line thickness (`0.35x` ~ `3.00x`) |
| `Left` / `Right` (Row 16) | SBC / Raw Line Width | Adjust terrain mesh / raw wireframe / ground ring line thickness (`0.35x` ~ `3.00x`) |
| **`TAB`** (or Row 17) | Monster Part Filter | Press `Left` / `Right` on this row to quick-switch partition mode; press `Space` / `Enter` to open the Monster Parts Inspector |
| `Left` / `Right` (Row 18) | Freeze Delayed Burst | Toggle World Freeze Delayed Burst (`ON` / `OFF`, default `ON`) |

#### 3. `[P]` → `[TAB]` Monster Parts Inspector Operation

- **Top Control Rows (Rows 0–4)**:
  - `PARTITION MODE`: Switch `OFF` / `HITZONE (FINE)` / `CLUTCH (SOFTEN)`
  - `UNSELECTED PARTS`: Switch unselected parts between `HIDE` and `GHOST OUTLINE`
  - `3D PART LABELS`: Switch 3D floating labels between `PHYS + ELEM + EXT` / `PHYS + EXTRACT` / `ID + EXTRACT` / `OFF`
  - `TARGET MONSTER`: Switch `AUTO (SOLO)` / `AUTO (+OTHERS)` / locked target `#1..#8`
  - `QUICK PART SELECT`: Press `Space` / `Enter` to toggle between All On and Solo; press `Left` / `Right` to step through parts one by one in Solo mode
- **Part List Rows**: Press `Space` / `Enter` / `Left` / `Right` to toggle the selected part; on keyboard, press `1`–`0`, `-`, `/`, `,`, `.` to quick-toggle the first 14 parts, or press **`=`** to solo / un-solo the highlighted part.
- **Return to Main Panel**: Press **`TAB`**, or move the cursor to the bottom row **`<<< BACK TO MAIN PANEL`** (press `Up` once from row 0 to wrap straight to it) and press `Space` / `Enter` / `A`.

#### 4. Controller Operation for the `[P]` Panel

When the `[P]` panel is open, you can control it directly with a controller (keyboard single-row number/symbol shortcuts do not apply to controllers):

| Controller Input | Equivalent Action |
|---|---|
| **D-Pad `Up` / `Down`** | Move cursor up/down (supports hold auto-repeat) |
| **D-Pad `Left` / `Right`** | Adjust the selected row's option or toggle |
| **`A` (Xbox `A` / PS `×`)** | Same as keyboard `Space` (toggle, step mode, enter/exit the Monster Parts sub-panel) |
| **Hold `LB`** | Temporarily passes controller input through so you can switch Radial Menu wheels and send shoutouts while the panel is open |

#### 5. In-Game Chat / Radial Shoutout Triggers

Supports triggering the 7 core main hotkeys plus World Freeze Delayed Burst via **exact full-string match** (works by typing directly in in-game chat or binding custom shoutouts to the Radial Menu):

| Main Hotkey | Action | Default Trigger String | `.ini` Key |
|---|---|---|---|
| **`U`** | Toggle hitbox & scene visualization | `$draw_hitbox` | `ChatCmdVis` |
| **`I`** | Full Freeze / Resume | `$full_freeze` | `ChatCmdFreeze` |
| **`K`** | World Freeze / Resume | `$ThE_WoRLD!!!` | `ChatCmdWorldFreeze` |
| **`O`** | Step +1 frame | `$step_frame` | `ChatCmdStep` |
| **`J`** | Toggle Keep Single-Trigger Hitboxes Active | `$keep_hitbox` | `ChatCmdKeepHitbox` |
| **`P`** | Open / close filter panel | `$show_panel` | `ChatCmdPanel` |
| **`\`** | Show / hide plugin 2D Overlay (clean screenshots) | `$show_overlay` | `ChatCmdOverlay` |
| `—` (`[P]` Row 18) | Toggle World Freeze Delayed Burst | `$freeze_burst` | `ChatCmdFreezeBurst` |

- **Case-insensitive for ASCII letters by default** (`ChatCaseSensitive=0`); change to `1` in `mhw_hfi.ini` to require strict case-sensitive matching.
- **Customization & Hot-Reload**: Edit and save `nativePC\plugins\mhw_hfi.ini` at any time while the game is running (save as UTF-8 when using Chinese/Japanese shoutouts). Changes take effect on the very next chat/shoutout or panel open without restarting the game. Leave any entry empty or set it to `NONE` to disable that individual command.

---

### Configuration File (`nativePC\plugins\mhw_hfi.ini`)

A default `mhw_hfi.ini` containing all default values is included in the archive. Every entry is optional and uses its default value if omitted:

| Option | Default | Description |
|---|---|---|
| `Enabled` | `1` | Master plugin toggle (`1` enabled / `0` disabled) |
| `KeyToggleVis` / `KeyToggleFreeze` / `KeyFreezeOthers` / `KeyPersistHitbox` / `KeyStepFrame` / `KeyCycleFilter` / `KeyToggleOverlay` | `U` / `I` / `K` / `J` / `O` / `P` / `\` | Customize the 7 main hotkeys (supports letters, digits, `F1`–`F24`, `NUMPAD0`–`9`, `\`, etc.) |
| `StartVisOn` | `0` | Whether visualization starts ON automatically at game launch |
| `KeepHitboxesActive` | `1` | Keep single-trigger hitboxes active by default during World Freeze |
| `WorldFreezeDelayedBurst` | `1` | World Freeze Delayed Burst: prevents hit/pool overflow crashes, keeps live VFX/sound/hitstop/shakes during freeze, and bursts damage numbers, VFX, shakes & FOV punch on resume |
| `BlockCameraKeys` | `1` | Block in-game `O / K / L / ;` camera rotation keys |
| `ShowHudBanner` | `1` | Show top-left HUD status banner |
| `ChatCommands` | `1` | Enable/disable chat & shoutout triggers (hot-reloadable) |
| `ChatCaseSensitive` | `0` | Whether chat commands are ASCII case-sensitive (hot-reloadable) |
| `ChatCmdVis` / `ChatCmdFreeze` / `ChatCmdWorldFreeze` / `ChatCmdStep` / `ChatCmdKeepHitbox` / `ChatCmdPanel` / `ChatCmdOverlay` / `ChatCmdFreezeBurst` | `$draw_hitbox` / `$full_freeze` / `$ThE_WoRLD!!!` / `$step_frame` / `$keep_hitbox` / `$show_panel` / `$show_overlay` / `$freeze_burst` | Customize the 8 chat/shoutout trigger strings (UTF-8, hot-reloadable) |
| `GamepadPanel` | `1` | Enable/disable controller navigation for the `[P]` panel (hot-reloadable) |
| `LineThickness` / `EntityLineScale` / `SbcLineScale` | `2.0` / `1.0` / `1.0` | Base line thickness and entity/terrain line width multipliers |
| `OpacityScale` / `SbcSurfaceOpacity` / `HdrNits` | `1.0` / `1.0` / `0` | Global opacity, semi-transparent terrain surface opacity, and HDR luminance (`0` = auto) |
| `CategoryMask` / `SbcRadiusMeters` | `0x1B` / `25` | Default enabled category bitmask and terrain display radius |
| `StartHideMonster` / `StartHideStage` / `StartBackdrop` | `0` / `0` / `0` | Default model hiding and pure collision canvas modes |
| `DepthGuideMode` / `SbcMeshTier` / `SpecialSbcStyle` | `0` / `1` / `2` | Default terrain display style, collision tier, and special layer style |
| `EmPartMode` / `EmUnselectedStyle` / `EmPartLabels` | `0` / `0` / `0` | Default monster part partition mode, unselected part style, and 3D label mode |
