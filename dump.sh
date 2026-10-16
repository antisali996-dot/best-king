#!/system/bin/sh
# =====================================================================
# dump.sh — 战局内存 dump 一键 (King_battle 简化版)
#   用法: su → sh /data/adb/JAY/dump.sh          (在战中直接 dump)
#         sh /data/adb/JAY/dump.sh 60            (先等 60s 再 dump)
# =====================================================================
K=/data/adb/JAY/King_battle.sh
[ -f "$K" ] || { echo "[X] /data/adb/JAY/King_battle.sh 不在"; exit 4; }
sh "$K" "${1:-0}"
