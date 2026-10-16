#!/bin/sh
# best-king 使用卡（当前设备侧已部署/待部署版）
# ============ 1. 推送(仅PC): 我来推 ============
# adb push King.sh /data/local/tmp/King.sh
# adb push King_battle.sh /data/local/tmp/King_battle.sh
# 然后在设备上:
#   su
#   cp /data/local/tmp/King.sh /data/adb/JAY/King.sh && chmod 777 /data/adb/JAY/King.sh
#   cp /data/local/tmp/King_battle.sh /data/adb/JAY/King_battle.sh && chmod 700 /data/adb/JAY/King_battle.sh
# ============ 2. 战中 dump ============
#   sh /data/adb/JAY/King_battle.sh        # 立即(需在战中)
#   sh /data/adb/JAY/King_battle.sh 300    #(Player进局后) 倒计时300s
# ============ 2.5 批量定向抓帧 (King_shot.sh) ============
#   adb push King_shot.sh /data/local/tmp/King_shot.sh
#   cp /data/local/tmp/King_shot.sh /data/adb/JAY/ && chmod 700 /data/adb/JAY/King_shot.sh
#   清单: echo "0x起点 0x长度 名字" > /data/adb/JAY/shot.list
#   sh /data/adb/JAY/King_shot.sh /data/adb/JAY/shot.list /data/adb/JAY/shot 2 3   # 2帧, 间隔3s
# ============ 2.7 一键快照 (King_snap.sh: 单文件→单日志) ============
#   进对局后: sh /data/adb/JAY/King_snap.sh [帧数=8] [间隔秒=3]
#   产物: /data/adb/JAY/live/live_*.log (英雄表+坐标时序+本地对象hex, 抓完只发这一个文件)
# ============ 3. 观察点扫描(断点流, 主v1.2: 字段写入者定位) ============
#   /data/adb/JAY/King.sh hwbp_scan <对象地址hex> <毫秒> [m]
#   m: 2=write 3=read_write 4=execute
#   例: /data/adb/JAY/King.sh hwbp_scan 0x7514484cf0 20000 2
#
# 16点覆盖: +0x130/0x138/0x140/0x158/0x170/0x188/0x1A0/0x1B8/0x200/0x218/0x230/0x250/0x268/0x278/0x280/0x2C8
# 输出每 hit: pc/lr/x0/x1/x2/x8/x19/x20  — 直接锁定写入者函数, 离线即可反注册算法
# ============ 4. PTE 执行断点(暂未封装, 备用) ============
# 头文件有 ptebp_set/get_records (可绕16点上限), 需要时我再写成命令
