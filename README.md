# MHW:I Debug Hitbox, Player Frame States & Freeze Mod (15.23, DX11 + DX12 + HDR)

实时显示玩家、武器、投射物与怪物的碰撞体（受击判定体 Hurtbox / 攻击判定体 Hitbox）、**玩家无敌帧/霸体帧/GP/见切帧状态与剩余帧数**，并提供**零延迟全局冻结**、**除玩家外全部暂停（World Freeze）**与**逐帧步进**调试插件。

## 安装方式（Stracker's Loader 插件）

1. 确保已安装 **Stracker's Loader**。
2. 将 `build\debug_hitbox.dll` 复制到游戏目录：
   ```text
   Monster Hunter World\nativePC\plugins\debug_hitbox.dll
   ```
   （如使用发布包 `out\debug_hitbox-15.23.zip`，直接解压到游戏根目录即可。）
3. 启动游戏后，进入标题画面左上角会短暂显示 6 秒 `DEBUG HITBOX READY` 提示条，同时在 `nativePC\plugins\debug_hitbox.log` 生成诊断日志。

## 默认热键

| 按键 | 功能 |
|---|---|
| **`U`** | 开关实时碰撞体 / 攻击判定体 / 玩家特殊帧状态显示 |
| **`I`** | **全局冻结 / 恢复** — 0ms 零延迟暂停整个游戏逻辑（暂停期间怪物受击框与激活攻击框保持常驻可见） |
| **`K`** | **世界冻结（除玩家外全部暂停）** — 怪物、怪物弹道/吐息特效、随从、环境生物与机关全部定格，**仅玩家自身与武器/投射物正常行动**，且定格怪物的受击肉质框、推挤盒与当前攻击判定框保持真实碰撞交互 |
| **`J`** | **单次触发判定框常驻开关（Keep Hitboxes Active）** — 默认开启（HUD 显示 `[J] KEEP:ON`）；在世界冻结下使原本“命中/防御一次即销毁结束”的火球、冰锥、落雷、投技等单次判定框保持常驻且可反复触发练习翻滚/见切/GP |
| **`O`** | **逐帧步进（+1 帧）** — 在 `I`（全局冻结）或 `K`（世界冻结）下均可精确推进 1 帧，且完全消除步进时的相机仰角漂移 |
| **`P`** | 循环切换显示过滤模式：`COMBAT`（默认：战斗受击+攻击）→ `HITBOX`（仅攻击判定）→ `HURTBOX`（仅受击肉质）→ `ALL`（含推挤盒与环境机关） |

> 按键仅在游戏窗口处于前台且未按住 `Ctrl`/`Alt` 时触发，并自动屏蔽热键按下瞬间对游戏键盘视角（如默认 `I`/`K` 视角上下键）的干扰。

### 可选配置文件 `nativePC\plugins\debug_hitbox.ini`

无需创建此文件即可使用默认配置；如需改键或微调 HDR 亮度/线宽，可在 `nativePC\plugins\debug_hitbox.ini` 放置：

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
ShowHudBanner=1
LineThickness=2.0
OpacityScale=1.0
HdrNits=0
```

- `HdrNits=0`：HDR 模式下自动跟随游戏内参考白点（Paper White）进行 **Rec.2020 + SMPTE ST.2084 PQ**（HDR10）或 **scRGB 线性亮度**编码，彻底解决 HDR 下线框以 10,000 nits 峰值亮度刺眼过曝的问题；也可手动指定具体尼特值（如 `HdrNits=160`）。

## 颜色与玩家帧状态图例

### 1. 攻击与怪物判定体

| 颜色 | 含义 |
|---|---|
| 🔴 **亮红色（加粗）** | **怪物/环境攻击判定体（EM-ATK）** — 近战爪击/甩尾、属性魔法/吐息/激光（`uMhEfc` / `cAttackParamEm`）、火球/冰柱等当前激活帧攻击框 |
| 🟡 **亮金色（加粗）** | **玩家阵营攻击判定体（PL-ATK）** — 14 种武器挥砍、弓箭、弩弹、猎虫、随从等当前激活帧攻击框 |
| 🟢 **翠绿色** | **怪物受击判定体（EM-HURT）** — 怪物全身部位受击框（`Monster + 0x6A8 / +0x7628`）及木桩/推车受击框，暂停时全程稳定显示 |
| 🟣 **淡紫色** | **纯推挤盒/其他碰撞（OTHER）** — 仅在 `ALL` 模式下显示 |

### 2. 玩家受击框动态帧状态配色（左上角 HUD 同步显示精确剩余帧数）

| 玩家受击框颜色 | HUD 状态标识 | 对应帧状态 |
|---|---|---|
| ⚪ **冰白色（加粗高亮）** | `IFRAME <N>F` / `NOHIT <N>F` | **无敌帧（Invincibility Frames）** — 翻滚/垫步回避无敌帧（含回避性能加成）、飞扑、倒地起身、看破斩无敌帧等 |
| 🟣 **靛紫蓝色（加粗）** | `GUARD POINT` / `GP+COUNTER` | **GP 防御判定帧（Guard Point）** — 盾斧变形 GP、长枪防反/盾突 GP、片手/重弩 GP 等 |
| 🟠 **烈焰橙色（加粗）** | `HYPER <N>F` / `ROCKSTEADY <N>S` | **强霸体帧（Hyper Armor）** — 大剑铁山靠、斩斧零解、太刀居合霸体、不动衣装等减伤抗击飞帧 |
| 🟢 **薄荷青绿色（加粗）** | `COUNTER` / `AUTO-EVADE <N>S` | **反击 / 自动回避触发窗** — 见切/防反触发窗、转身衣装自动回避就绪状态 |
| 🔵 **深蔚蓝色** | `GUARD` | **常规防御（Guard）** — 举盾防御生效帧 |
| 🩷 **品红粉色** | `SUPER <N>F` / `SUPER ARMOR` | **普通霸体帧（Super Armor）** — 招式自带耐冲/抗小硬直帧 |
| 🩵 **青蓝色** | `NORMAL` | **常规受击状态** — 无特殊防御帧 |

## 编译方式

需要 Visual Studio 2022（Desktop development with C++）：

```batch
mods\debug_hitbox\build.bat
```

产物：`mods\debug_hitbox\build\debug_hitbox.dll`（发布包：`out\debug_hitbox-15.23.zip`，仅含 `nativePC/plugins/debug_hitbox.dll`）。
