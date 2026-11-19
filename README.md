# MHW:I Debug Hitbox Mod (15.23)

[中文说明](#中文说明) | [English Guide](#english-guide)

---

## 中文说明

面向《怪物猎人：世界 冰原》（Steam 15.23）的实时判定框可视化、怪物部位肉质/软化分区检视、地图地形碰撞查看与零延迟时停步进工具，兼容 DirectX 11 / DirectX 12 以及 SDR / HDR 显示。

### 前置要求与安装

1. 游戏版本：**Monster Hunter: World Iceborne 15.23**（Steam）。
2. 前置插件：**Stracker's Loader**。
3. 将压缩包内的 `nativePC` 文件夹解压至游戏根目录，确认最终路径为：
   ```text
   <游戏根目录>\nativePC\plugins\debug_hitbox.dll
   <游戏根目录>\nativePC\plugins\debug_hitbox.ini
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
- **全局冻结（Full Freeze）**：瞬时零延迟定格整个游戏，冻结期间可自由调整面板筛选与观察细节。
- **世界冻结（World Freeze）**：仅暂停怪物、弹道/吐息特效、随从与机关，玩家与武器照常自由行动，便于测试招式判定、打点、翻滚无敌帧与 GP/见切。
- **单次判定框常驻（Keep Hitboxes Active）**：世界冻结下保持火球、吐息、单次近战/投技等判定框在命中或防御后不消失，可反复练习翻滚、见切与 GP。
- **逐帧步进（+1 Frame）**：在全局冻结或世界冻结下精确推进 1 帧。

#### 5. 游戏内聊天 / 快捷轮盘定型文触发与手柄操控面板
- **聊天 / 轮盘定型文激活**：通过游戏内聊天或快捷轮盘定型文触发 6 个核心功能，绕开手柄按键紧张的限制；仅响应本地玩家主动发言，支持连续重复发送同一条定型文，且支持在游戏运行中修改 `debug_hitbox.ini` 后免重启热刷新。
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

> - 打开游戏内聊天框、搜索框或起名输入文字时，插件会自动暂停拦截全部热键。
> - 插件启用期间默认屏蔽游戏原生 `O / K / L / ;` 键盘转视角键（打字时自动放行），避免按 `K` / `O` 时误转镜头；可在 `debug_hitbox.ini` 中将 `BlockCameraKeys` 设为 `0` 恢复。

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

仅支持触发 6 个核心主热键，采用**完整字符串精确匹配**（可在游戏内直接打字发送，或填入自定义定型文并绑到快捷轮盘）：

| 对应主热键 | 功能 | 默认触发字符串 | `.ini` 配置项 |
|---|---|---|---|
| **`U`** | 开关判定框与场景视图 | `$draw_hitbox` | `ChatCmdVis` |
| **`I`** | 全局冻结 / 恢复 | `$full_freeze` | `ChatCmdFreeze` |
| **`K`** | 世界冻结 / 恢复 | `$ThE_WoRLD!!!` | `ChatCmdWorldFreeze` |
| **`O`** | 单帧步进 +1 帧 | `$step_frame` | `ChatCmdStep` |
| **`J`** | 开关单次判定框常驻 | `$keep_hitbox` | `ChatCmdKeepHitbox` |
| **`P`** | 打开 / 关闭筛选面板 | `$show_panel` | `ChatCmdPanel` |

- **默认不区分英文大小写**（`ChatCaseSensitive=0`），可在 `debug_hitbox.ini` 中改为 `1` 开启严格区分大小写。
- **自定义与热刷新**：游戏运行期间随时编辑并保存 `nativePC\plugins\debug_hitbox.ini`（中文/日文定型文请保存为 UTF-8 编码），下一次发聊天/定型文或打开面板时即时生效，无需重启游戏。将任意一项留空或填 `NONE` 可单独禁用该命令。

---

### 配置文件（`nativePC\plugins\debug_hitbox.ini`）

压缩包中已附带包含全部默认值的 `debug_hitbox.ini`。所有配置项均可选，缺省时使用默认值：

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `Enabled` | `1` | 插件总开关（`1` 启用 / `0` 禁用） |
| `KeyToggleVis` / `KeyToggleFreeze` / `KeyFreezeOthers` / `KeyPersistHitbox` / `KeyStepFrame` / `KeyCycleFilter` | `U` / `I` / `K` / `J` / `O` / `P` | 自定义 6 个主热键（支持字母、数字、`F1`~`F24`、`NUMPAD0`~`9` 等） |
| `StartVisOn` | `0` | 启动游戏后默认是否自动开启视图 |
| `KeepHitboxesActive` | `1` | 世界冻结下默认开启单次判定框常驻 |
| `BlockCameraKeys` | `1` | 屏蔽游戏 `O / K / L / ;` 转视角键 |
| `ShowHudBanner` | `1` | 显示左上角状态提示条 |
| `ChatCommands` | `1` | 开关聊天/定型文触发（支持热刷新） |
| `ChatCaseSensitive` | `0` | 聊天命令是否区分英文大小写（支持热刷新） |
| `ChatCmdVis` / `ChatCmdFreeze` / `ChatCmdWorldFreeze` / `ChatCmdStep` / `ChatCmdKeepHitbox` / `ChatCmdPanel` | `$draw_hitbox` / `$full_freeze` / `$ThE_WoRLD!!!` / `$step_frame` / `$keep_hitbox` / `$show_panel` | 自定义 6 个聊天/定型文触发词（UTF-8，支持热刷新） |
| `GamepadPanel` | `1` | 开关 `[P]` 面板手柄操控（支持热刷新） |
| `LineThickness` / `EntityLineScale` / `SbcLineScale` | `2.0` / `1.0` / `1.0` | 基础线宽与实体/地形线宽倍率 |
| `OpacityScale` / `SbcSurfaceOpacity` / `HdrNits` | `1.0` / `1.0` / `0` | 全局透明度、地形半透明面透明度、HDR 亮度（`0` 为自动） |
| `CategoryMask` / `SbcRadiusMeters` | `0x1B` / `25` | 初始显示类别掩码与地形显示半径 |
| `StartHideMonster` / `StartHideStage` / `StartBackdrop` | `0` / `0` / `0` | 初始模型隐藏与纯碰撞画布模式 |
| `DepthGuideMode` / `SbcMeshTier` / `SpecialSbcStyle` | `0` / `1` / `2` | 初始地形风格、碰撞层级与特殊层样式 |
| `EmPartMode` / `EmUnselectedStyle` / `EmPartLabels` | `0` / `0` / `0` | 初始怪物部位分区、未选中样式与 3D 标签模式 |

---

## English Guide

A real-time collision/hitbox visualizer, monster part hitzone & Clutch Claw zone inspector, stage terrain collision viewer, and zero-delay frame freeze/stepping tool for **Monster Hunter: World Iceborne (Steam 15.23)**, supporting both DirectX 11 / DirectX 12 and SDR / HDR displays.

### Requirements & Installation

1. **Game Version**: Monster Hunter: World Iceborne **15.23** (Steam).
2. **Prerequisite**: **Stracker's Loader**.
3. Extract the `nativePC` folder from the archive into your game root directory:
   ```text
   <MHW Root>\nativePC\plugins\debug_hitbox.dll
   <MHW Root>\nativePC\plugins\debug_hitbox.ini
   ```

---

### Features

#### 1. Real-Time Hitbox, Hurtbox, Pushbox & Terrain Visualization
Toggle 8 independent categories individually or switch with presets:
- **Player Hurtbox + Defensive Frame State**: Color-coded live status with remaining frames/seconds for Invincibility (evade/action i-frames), Guard Point, Guard, Hyper Armor (including Rocksteady Mantle), Counter / Foresight windows, and Super Armor.
- **Player Hitboxes**: Melee weapons, ammo/shells, Kinsect, and Clutch Claw attacks (gold).
- **Palico / Companions**: Dedicated hurtboxes (sky blue) and hitboxes (amber).
- **Monster Hurtboxes**: Full-body hurtboxes (emerald green by default; color-coded and filterable per part in the Monster Parts Inspector).
- **Monster Hitboxes**: Melee attacks, breaths, beams, projectiles, and stage hazards (bright red).
- **Body Pushboxes**: Physical player and monster anti-clipping collision volumes (magenta).
- **Map Interactive Entities**: Training pole/cart, barrel bombs, falling boulders, traps, and endemic life (mint hurtboxes / coral hitboxes).
- **Map Terrain Collision Mesh**: Static and dynamic terrain collision meshes within `15m / 25m / 45m / 80m`, with 6 visual styles (semi-transparent 3D, opaque 3D solid, ground distance rings, raw wireframe), Low/High collision tier filtering, 6-step opacity for special non-solid layers (water, zone boundaries, camera blockers), and independent entity & terrain line width scaling.

#### 2. Monster Parts Inspector (Live Hitzones, Tenderize & Kinsect Extract)
Designed for deep monster study, filtering both hurtboxes and matching body pushboxes together:
- **Fine Hitzone Partition (`HITZONE`)**: Color-codes each anatomical part with live data:
  - Color-coded Kinsect Extract `[R/W/O/G]`
  - Live Physical Hitzones `S/B/A` (Sever / Blunt / Shot, updating live on state changes, part breaks, and Clutch Claw tenderizing with `*` and warm-gold highlight)
  - Color-coded Elemental Hitzones `F/W/T/I/D` (Fire / Water / Thunder / Ice / Dragon)
  - Per-part hurtbox + pushbox counts `(nH+mP)` and an independent center helper sphere slot (`AUX CTR`)
- **Clutch Claw Soften Zone Partition (`CLUTCH`)**: Groups hurtboxes by distinct tenderizable Clutch Claw zones with separate colors; unclutchable parts are grouped into a dedicated `NO-CLAW` slot.
- **Flexible Filtering**: Multi-toggle any combination of parts, one-key `SOLO` isolation and sequential part stepping, `HIDE` vs `GHOST OUTLINE` for unselected parts, multi-monster target locking, and 4 modes of 3D floating part labels.

#### 3. Scene / Model Hiding & Pure Collision Canvas
All modes keep the in-game HUD/UI clearly visible and interactions working:
- **Hide Monster Mesh**: Hide monster models and shadows to focus purely on collision volumes.
- **Hide Map / Stage Mesh**: `KEEP INTERACT` (hide static terrain/foliage while keeping gathering points, wedge beetles, camp boxes, and gimmicks visible) or `HIDE ALL MAP` (hide all stage visuals while keeping interactions active).
- **Pure 3D / Canvas Backdrop**: `HIDE PL/VFX` (hide player, weapon, Palico, and 3D VFX), `HIDE ALL 3D` (pure dark collision canvas with full HUD intact), or `DIM 45%` (softly dim 3D world while keeping UI at full brightness).

#### 4. Zero-Delay Freeze, World Freeze & Frame Stepping
- **Full Freeze**: Instant pause of the entire game while still allowing panel adjustments and camera/overlay inspection.
- **World Freeze**: Freezes monsters, projectiles/breath effects, companions, and gimmicks while the player remains completely free to move and practice timing, spacing, i-frames, and counters.
- **Keep Hitboxes Active**: Keeps single-trigger hitboxes (fireballs, beams, grabs, single-hit swings) active and repeatable during World Freeze for repeated dodge/GP/Foresight practice.
- **Single-Frame Step (+1 Frame)**: Advances exactly 1 frame in either Full Freeze or World Freeze.

#### 5. In-Game Chat / Radial Shoutout Triggers & Controller Panel Control
- **Chat & Radial Menu Shoutouts**: Trigger the 6 core actions via exact string match in in-game chat or Radial Menu custom shoutouts—ideal for controller users. Responds only to local player messages, supports consecutive repeated triggers, and hot-reloads changes from `debug_hitbox.ini` without restarting the game.
- **Controller Panel Navigation**: Operate the `[P]` panel directly using controller D-Pad + `A` (PS `×`). Panel controls are isolated from gameplay while open, and automatically pass through whenever holding `LB` (for Radial Menu) or typing in chat.

---

### Controls

#### 1. Main Keyboard Hotkeys (Default)

| Key | Action |
|---|---|
| **`U`** | Master toggle: show / hide collision & scene overlay |
| **`I`** | Full Freeze / Resume |
| **`K`** | World Freeze / Resume (everything paused except the player) |
| **`J`** | Toggle Keep Single-Trigger Hitboxes Active (default ON) |
| **`O`** | Step +1 frame (while Full or World Freeze is active) |
| **`P`** | Open / close the interactive Filter & Scene Panel |
| **`Shift + P`** | Quick-cycle visibility presets without opening the panel |

> - All mod hotkeys pause automatically while typing in in-game chat, search, or naming inputs.
> - In-game keyboard camera rotation keys (`O / K / L / ;`) are blocked outside text input by default so `K` / `O` never nudge the camera (set `BlockCameraKeys=0` in `debug_hitbox.ini` to disable).

#### 2. Interactive `[P]` Panel Controls (Main Page)

With the panel open (`P`):
- **`Up` / `Down`**: Move cursor (supports hold auto-repeat)
- **`Left` / `Right`**: Adjust value / cycle mode on the selected row
- **`Space` / `Enter`**: Toggle or step the selected row (row 17 opens the Monster Parts Inspector)
- **`TAB`**: Switch between Main Panel and Monster Parts Inspector
- **`P` / `ESC` / `Backspace`**: Close panel
- **Direct Keyboard Shortcuts**: `1`–`8` toggle the 8 collision categories, `9` hides monster mesh, `0` cycles stage mesh hiding, `-` cycles backdrop/canvas mode, `/` cycles terrain display style, `,` cycles Low/High terrain tier, `.` cycles special terrain layer opacity, `=` flips between `PL+EM ONLY` and `MAP ONLY`. Rows 15 & 16 adjust entity and terrain line width (`0.35x`–`3.00x`).

#### 3. `[P]` → `[TAB]` Monster Parts Inspector

- **Control rows (0–4)**: Partition Mode (`OFF` / `HITZONE` / `CLUTCH`), Unselected Parts (`HIDE` / `GHOST OUTLINE`), 3D Part Labels, Target Monster (`AUTO (SOLO)` / `AUTO (+OTHERS)` / `LOCK #1..#8`), and Quick Part Select (`Space`/`Enter` toggles All vs Solo; `Left`/`Right` steps through parts one by one in Solo mode).
- **Part rows**: `Space` / `Enter` / `Left` / `Right` toggles the highlighted part; keyboard `1`–`0`, `-`, `/`, `,`, `.` toggle the first 14 parts directly; `=` toggles Solo on the highlighted part.
- **Return to Main Panel**: Press **`TAB`**, or move to the bottom **`<<< BACK TO MAIN PANEL`** row (press `Up` once from row 0 to wrap directly to it) and press `Space` / `Enter` / `A`.

#### 4. Controller Controls for the `[P]` Panel

When the `[P]` panel is open (keyboard single-row shortcuts do not apply to controllers):

| Controller Input | Equivalent Action |
|---|---|
| **D-Pad `Up` / `Down`** | Move cursor (supports hold auto-repeat) |
| **D-Pad `Left` / `Right`** | Adjust value / step mode |
| **`A` (Xbox) / `Cross` (PS)** | Same as `Space` (toggle, step, enter/exit Monster Parts page) |
| **Hold `LB`** | Temporarily passes controller input to the game so you can switch Radial Menu wheels and trigger shoutouts while the panel is open |

#### 5. In-Game Chat & Radial Shoutout Commands

Exact string match triggers ONLY the 6 main actions (works with typed chat and Radial Menu custom shoutouts):

| Action | Default Chat / Shoutout Command | `debug_hitbox.ini` Key |
|---|---|---|
| **`U`** — Toggle Hitbox / Overlay | `$draw_hitbox` | `ChatCmdVis` |
| **`I`** — Full Freeze / Resume | `$full_freeze` | `ChatCmdFreeze` |
| **`K`** — World Freeze / Resume | `$ThE_WoRLD!!!` | `ChatCmdWorldFreeze` |
| **`O`** — Step +1 Frame | `$step_frame` | `ChatCmdStep` |
| **`J`** — Keep Hitboxes Active | `$keep_hitbox` | `ChatCmdKeepHitbox` |
| **`P`** — Open / Close Panel | `$show_panel` | `ChatCmdPanel` |

- Case-insensitive for ASCII letters by default (`ChatCaseSensitive=0`; set to `1` for strict case-sensitive match).
- **Hot-Reload**: Edit and save `nativePC\plugins\debug_hitbox.ini` at any time while the game is running (save as UTF-8 for non-ASCII shoutouts). New command strings take effect on the very next chat message or panel open—no restart required. Set any command to empty or `NONE` to disable it.

---

### Configuration (`nativePC\plugins\debug_hitbox.ini`)

A complete default `debug_hitbox.ini` is bundled with the release. All entries are optional and fall back to defaults if omitted:
- **Hotkeys**: `KeyToggleVis`, `KeyToggleFreeze`, `KeyFreezeOthers`, `KeyPersistHitbox`, `KeyStepFrame`, `KeyCycleFilter`
- **Behavior**: `Enabled`, `StartVisOn`, `KeepHitboxesActive`, `BlockCameraKeys`, `ShowHudBanner`
- **Chat & Controller (Hot-Reloadable)**: `ChatCommands`, `ChatCaseSensitive`, `ChatCmdVis`, `ChatCmdFreeze`, `ChatCmdWorldFreeze`, `ChatCmdStep`, `ChatCmdKeepHitbox`, `ChatCmdPanel`, `GamepadPanel`
- **Appearance & Defaults**: `LineThickness`, `EntityLineScale`, `SbcLineScale`, `OpacityScale`, `SbcSurfaceOpacity`, `HdrNits`, `CategoryMask`, `SbcRadiusMeters`, `StartHideMonster`, `StartHideStage`, `StartBackdrop`, `DepthGuideMode`, `SbcMeshTier`, `SpecialSbcStyle`, `EmPartMode`, `EmUnselectedStyle`, `EmPartLabels`
