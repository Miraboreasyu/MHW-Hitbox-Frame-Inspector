# MHW:I Debug Hitbox & Freeze Mod (15.23, DX11 + DX12 + HDR)

纯粹独立的实时碰撞体 / 攻击判定体（Hitbox）/ 受击判定体（Hurtbox）/ **玩家无敌帧·霸体帧·GP·见切帧状态与剩余帧数**可视化及 **零延迟全局/世界冻结与逐帧步进** 插件（`debug_hitbox.dll`）。

> **职责拆分说明**：TAS 逐帧录制/回放、确定性 Checkpoint 与条件自动时停已完全独立拆分至平行项目 `mods\debug_tas`（产物 `nativePC\plugins\debug_tas.dll` / `out\debug_tas-15.23.zip`）。平时只需要看判定框或时停练习时，仅安装 `debug_hitbox.dll` 即可。

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
| **`U`** | 开关实时碰撞体 / 攻击判定体 / 玩家特殊帧状态显示 |
| **`I`** | **全局冻结 / 恢复** — 0ms 零延迟暂停整个游戏逻辑 |
| **`K`** | **世界冻结（除玩家外全部暂停）** — 怪物、弹道/吐息特效、随从与机关定格，仅玩家与武器正常行动 |
| **`J`** | **单次触发判定框常驻开关（Keep Hitboxes Active）** — 默认开启（`[J] KEEP:ON`） |
| **`O`** | **逐帧步进（+1 帧）** — 在 `I` 或 `K` 冻结下精确推进 1 帧 |
| **`P`** | 循环切换显示过滤模式：`COMBAT` → `HITBOX` → `HURTBOX` → `ALL` |

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
ShowHudBanner=1
LineThickness=2.0
OpacityScale=1.0
HdrNits=0
```

## 待机完全原生零开销架构（彻底消除 1% Low 掉帧）

1. **0 后台轮询线程、0 每帧 `win32k` 系统调用**：初始化完成后不保留任何后台线程；通过进程内窗口线程专属 `WH_KEYBOARD` 事件钩子响应热键，彻底消除每帧 `GetAsyncKeyState` / `GetForegroundWindow` 在高回报率鼠标下引发的 `win32k` 内核锁竞争与跨线程抢占。
2. **正常游玩时 `.text` 模拟函数 100% 原生机器码（0 Hook）**：启动时**仅**挂载 `cD3DRender::present`（每帧 1 次）；每秒调用上百万次的 `cUnit::genericMove`（`0x142228F50`）与碰撞派发 `HitDispatch`/`GuardDispatch` 在正常游玩（乃至开启 `U` 显示判定框时）**完全不打补丁、保持 100% 原版字节**，仅在按下 `K`/`I`/`O` 冻结期间通过 64 位硬件原子指令 `LOCK CMPXCHG8B` 按需挂载，解冻后立即原子恢复原版机器码，零 I-TLB / BTB 抖动。
3. **`Present` 待机 1 字节无栈帧尾跳转**：未开启显示且未冻结时，`Hook_D3DPresent` 仅执行单条 `cmp byte ptr [g_presentWorkMask], 0` 后直接尾跳转回原函数（每帧仅约 3 纳秒，0 锁、0 D3D 调用、0 游戏内存读取）。

## 编译方式

```batch
mods\debug_hitbox\build.bat
```

产物：`mods\debug_hitbox\build\debug_hitbox.dll`（发布包：`out\debug_hitbox-15.23.zip`，仅含 `nativePC/plugins/debug_hitbox.dll`）。
