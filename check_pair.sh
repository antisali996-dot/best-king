#!/system/bin/sh
# check_pair.sh: 校验 dump(13:53) 时进程 与 现时进程 是否同一 ASLR 世代
D=/data/adb/JAY
echo "== 现时 sgame 主进程 =="
M=$(ps -A | grep -m1 ' com.tencent.tmgp.sgame' | awk '{print $2}')
echo "pid=$M"
grep -m1 libGameCore /proc/$M/maps
echo "== dump 时 (game.idx #MAP) libGameCore 行 =="
grep libGameCore $D/game.idx | head -1
echo "== dump 时 recarray 附近报价 =="
grep -m3 '^' $D/game.idx | tail -2
