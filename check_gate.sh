#!/system/bin/sh
# 一次性自检: King.sh 闸门 + 游戏进程 + 磁盘(无副作用)
sh /data/adb/JAY/King.sh selftest 2>&1 | tail -3
pgrep -f com.tencent.tmgp.sgame >/dev/null && echo GAME_RUNNING || echo GAME_NOT_RUNNING
df -k /data | tail -1 | awk '{print $4"KB free"}'
