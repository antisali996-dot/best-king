# best-king

独立内存 dump 工具 —— **Paradise（par）内核驱动专用**，带**硬闸门**。

> 驱动连接不成功 → 主程序一行都不执行（以退出码 `11` 阻断）。

## 最常用：MT管理器一键（无参数自动模式）

在 MT管理器里**直接"运行" King.sh（不带任何参数）**：

1. 自动连接 par 驱动（失败即阻断，主程序不启动）
2. 自动获取「王者荣耀」进程（`com.tencent.tmgp.sgame`，内核层解析）
3. 自动 dump → `/data/adb/JAY/game.bin` + `/data/adb/JAY/game.idx`

全程零输入；对局内执行效果最佳。

## 硬闸门流程（启动即执行）

| 步骤 | 内容 | 失败结果 |
|---|---|---|
| [1/4] | 构造 par 驱动连接（内核 fd 安装） | `[X]` + 阻断 |
| [2/4] | 自检读：经驱动读本进程 canary，验证整条链路 | `[X]` + 阻断 |
| [3/4] | 内核层解析目标 pid（`pidof` 兜底） | `[X]` + 阻断 |
| [4/4] | 目标读验证（maps 首个可读区试读） | `[X]` + 阻断 |

全部通过后打印：

```
[OK] ★ par 驱动连接成功 ★  (自检读通过 canary=0x7061725F4F4B3131)
...
[OK] ★ 前置检查全部通过 ★  目标=com.tencent.tmgp.sgame  pid=xxxxx  读验证=OK
```

之后才进入主程序（所有 dump 功能）。

## 命令

```
King.sh [--target pkg|pid] [--chunk N] <命令> ...
  selftest                              驱动连接自检（闸门流程单跑）
  mods   [过滤]                          模块列表
  maps   [过滤]                          maps 打印
  range  <hex起始> <hex长度> <out.bin>   任意区间 dump（上限 8GB/次）
  dumpgame <out.bin> <out.idx>           一键圈选匿名堆区+[anon:.bss]
                                         （单区截断 256MB / 总量截断 1500MB /
                                          索引含完整 maps 副本, "#MAP " 前缀行）
默认目标: com.tencent.tmgp.sgame
```

## 构建 / 部署

```bat
build_push.cmd
```

- 需要 NDK r26d；NDK 路径解析：`%BESTKING_NDK%` → `..\xiaogu-kernel\tools\_vendor\android-ndk-r26d`
- 产物：`libs\arm64-v8a\King.sh`，自动推送到 `/data/adb/JAY/King.sh`

拉取产物（二进制安全）：

```bat
pull.cmd game.bin D:\Download\game.bin
pull.cmd game.idx D:\Download\game.idx
```

## 典型用法（对局内 dump 全量分析内存）

```bash
D=/data/adb/JAY/King.sh
su -c "$D selftest"      # 先验证闸门
su -c "$D dumpgame /data/adb/JAY/game.bin /data/adb/JAY/game.idx"
```

## 批量定向抓帧（King_shot.sh）

多帧小快照场景（走位验证 / 受伤差分）用 `King_shot.sh`：一次调用抓"清单里所有区间"为一帧，
支持连续多帧 + 间隔；自动去 CR、跳过注释行、逐段报 OK/FAIL，失败可手动复跑。

```bash
# 清单(每行: hex起址 hex长度 名字; 空行/# 注释忽略):
#   0x758dca6000 0x2800 heroX2free
#   0x758dde0000 0x20000 pArena
sh /data/adb/JAY/King_shot.sh /data/adb/JAY/shot.list /data/adb/JAY/shot 2 3
#                              清单                 输出目录          2帧 间隔3s
```

产物: `<输出目录>/f01_<名字>.bin, f02_...`；退出码 0=全成功 3=有失败段 2=参数问题 4=King.sh缺失。
注意: 每段仍是独立 King.sh 进程（各走一次闸门），段与段之间约 0.3~1s；站桩场景无影响。

## 一键快照（King_snap.sh）

进对局后执行；自动定位英雄链并采样 N 帧，输出**单个日志**：每帧十英雄的
uid/id/camp/slot/坐标（文本）+ 本地英雄 P/X1/X2/T/A/C1..C4 原始十六进制（离线差分找血量用）。

```bash
sh /data/adb/JAY/King_snap.sh            # 默认 8 帧 × 3 秒
sh /data/adb/JAY/King_snap.sh 12 2       # 12 帧 × 2 秒
```

产物: `/data/adb/JAY/live/live_<时间>_<pid>.log`（约 100KB/帧；抓完只发回这一个文件即可）。
运行期间在游戏内正常走位/挨打即可；退出码 0=成功 3=未进对局/链路失败 4=缺 King.sh。
环境要求: 至少一个支持大整数输出的 awk（设备自带 busybox 即可；脚本会自动定位并自检）。
日志语法: 英雄表 `[i] uid id camp slot P X1 x z`；每帧 `POS <idx> <uid> <id> <camp> <x> <z>`
（坐标读取失败时为 `POS <idx> <uid> <id> <camp> ERR ERR`）；
数据块 `HEX <名称> 0x<地址> 0x<长度>`（名称 P/X1/X2/T/A/C1..C4，后跟 od 十六进制行），
异常块为 `HEX <名称> SKIP`（指针无效）或 `HEX <名称> 0x<地址> 0x<长度> FAIL`（读取失败/缺文件）。
退出码: 0=成功 2=参数/环境问题 3=未进对局/链路失败 4=缺 King.sh（信号中止 130/143）。

## 说明

- 全内存读通道只经 par 驱动 ioctl，无任何旁路回退；读 `/proc/<pid>/maps` 仅用于取地址区间（文件读，非内存读）。
- `third_party/paradise/` 内的 `paradise_api.h` / `libparadise_api.a` 为 Paradise v2 驱动 API（arm64-v8a 预编译库）。
- 运行需要 root + 已刷入 Paradise 驱动；`/proc/<pid>/maps` 需要可读（root 下默认满足）。
