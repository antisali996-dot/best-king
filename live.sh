#!/system/bin/sh
# =====================================================================
# live.sh — 王者字段抓取一键 (bplive 全自动版)
#   用法: su → sh /data/adb/JAY/live.sh      (开着它打一局即可)
#   依赖: /data/adb/JAY/King.sh (v1.5+)
# =====================================================================
K=/data/adb/JAY/King.sh
[ -f "$K" ] || { echo "[X] /data/adb/JAY/King.sh 不在"; exit 4; }
D=600000
M=2
[ -n "$1" ] && [ "$1" -ge 0 ] 2>/dev/null && D=$1
[ -n "$2" ] && [ "$2" -ge 0 ] 2>/dev/null && M=$2
exec "$K" bplive "$D" "$M"
