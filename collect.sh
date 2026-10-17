#!/system/bin/sh
# ============================================================================
# collect.sh — 对局自动采集（一次运行, 全自动时间表; 用户零操作）
# ----------------------------------------------------------------------------
# 用法: sh /data/adb/JAY/collect.sh
# 时间表（以"确认进入对局"为基准 T0, 约等于对局 0:10-0:45）:
#   T0      等待进对局（King_snap 1帧轮询, 最长5分钟）
#   T0+ 70s arena 时序列A  12帧×3s  (覆盖对局 ~2:00 施法窗)
#   T0+195s arena 时序列B  12帧×3s  (覆盖对局 ~4:00 施法窗)
#   T0+275s dumpgame 1               (对局 ~5:00, 兵线/野怪在场上)
#   T0+615s dumpgame 2               (对局 ~10:40, 与dump1差分找对局时钟)
# 产物: /data/adb/JAY/auto/<stamp>/  (collect.log + arenaA/B/*.bin + game1/2)
# 退出码: 0=完成 2=环境问题 3=未进入对局
# ============================================================================

set -f

OUT=/data/adb/JAY/auto/$(date '+%y%m%d_%H%M%S')
mkdir -p "$OUT" 2>/dev/null || { echo "[X] 无法创建输出目录: $OUT"; exit 2; }
LOG="$OUT/collect.log"
say() { echo "$@" >> "$LOG"; echo "$@"; }

# ---- awk 定位（必须支持 48 位整数; 与 King_snap 相同策略）----
awk_ok() {
    [ "$("$@" 'BEGIN{print 4294967296+1}' 2>/dev/null)" = "4294967297" ] || return 1
    return 0
}
AWK=""
if awk_ok awk; then AWK="awk"
elif awk_ok /data/adb/magisk/busybox awk; then AWK="/data/adb/magisk/busybox awk"
elif awk_ok /system/bin/busybox awk; then AWK="/system/bin/busybox awk"
else
    echo "[X] 未找到支持大整数的 awk"; exit 2
fi

h2d() {
    $AWK -v h="$1" 'BEGIN{
        h=toupper(h); sub(/^0X/,"",h); v=0; n=split(h,c,"");
        if(n<1){print "ERR"; exit}
        for(i=1;i<=n;i++){d=index("0123456789ABCDEF",c[i])-1; if(d<0){print "ERR"; exit}; v=v*16+d}
        print v
    }'
}
d2h() {
    $AWK -v v="$1" 'BEGIN{
        n=v+0; if(n<0){print "ERR"; exit}
        if(n==0){print "0x0"; exit}
        h=""; while(n>0){d=n%16; h=substr("0123456789ABCDEF",d+1,1) h; n=int((n-d)/16)}
        print "0x" h
    }'
}

# ---- 定位 King.sh / linker64 ----
K=""
SELFDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
for cand in "$SELFDIR/King.sh" /data/adb/JAY/King.sh; do
    [ -f "$cand" ] && { K=$cand; break; }
done
[ -z "$K" ] && { echo "[X] 未找到 King.sh"; exit 2; }
LR=/system/bin/linker64
[ -x "$LR" ] || LR=""

KEXEC() { # KEXEC <args...>  (linker64 装载执行 King.sh)
    if [ -n "$LR" ]; then "$LR" "$K" "$@" </dev/null
    else "$K" "$@" </dev/null; fi
}

# ---- 预检: 依赖脚本必须在位 ----
for f in /data/adb/JAY/King.sh /data/adb/JAY/King_snap.sh /data/adb/JAY/King_shot.sh; do
    if [ ! -f "$f" ]; then
        echo "[X] 缺少依赖: $f"
        exit 2
    fi
done
FAILED=0

say "=================================================="
say "collect · $(date '+%Y-%m-%d %H:%M:%S')"
say "输出: $OUT"
say "=================================================="

# ---- 1) 等到进入对局 ----
say "[1] 等待进入对局（最长5分钟）..."
T0=""
i=0
while [ $i -lt 60 ]; do
    SN="$OUT/snap_wait"
    mkdir -p "$SN"
    sh /data/adb/JAY/King_snap.sh 1 0 "$SN" >"$OUT/snap_wait.out" 2>&1
    SLOG=$(ls -t "$SN"/live_*.log 2>/dev/null | head -1)
    if [ -n "$SLOG" ]; then
        SSZ=$(wc -c < "$SLOG" 2>/dev/null)
        if [ -n "$SSZ" ] && [ "$SSZ" -ge 100000 ] 2>/dev/null; then
            T0FILE="$SLOG"
            T0=$(date +%s)
            say "[1] 对局确认 $(date '+%H:%M:%S')  (snap=$SLOG)"
            break
        fi
    fi
    i=$((i+1))
    sleep 5
done
if [ -z "$T0" ]; then
    say "[X] 5分钟内未确认进入对局, 退出"
    exit 3
fi

# ---- 2) 从 snap 日志解析本地 P 地址 -> arena 区间 ----
PLINE=$(grep -a '本地英雄' "$T0FILE" 2>/dev/null | head -1)
LPHEX=$(echo "$PLINE" | $AWK '{for(i=1;i<=NF;i++){if($i ~ /^P=0x/){sub(/^P=/,"",$i); print $i; exit}}}')
if [ -z "$LPHEX" ]; then
    say "[X] 无法解析本地英雄 P 地址: $PLINE"
    exit 3
fi
LP=$(h2d "$LPHEX")
LPOK=$($AWK -v a="$LP" 'BEGIN{print (a>481036337152 && a<549755813888) ? 1 : 0}')
[ "$LPOK" = "1" ] || { say "[X] P 地址非法: $LPHEX -> $LP"; exit 3; }
ASTART_D=$( $AWK -v a="$LP" 'BEGIN{printf "%.0f", a-3145728}' )      # LP-0x300000
ASTART=$(d2h "$ASTART_D")
ASLEN=$(echo "$ASTART" | sed 's/^0x//')
case "$ASLEN" in ''|ERR|*[!0-9A-Fa-f]*) say "[X] arena 起点异常: $ASTART"; exit 3;; esac
[ "${#ASLEN}" -ge 9 ] || { say "[X] arena 起点过短: $ASTART"; exit 3; }
say "[2] 本地P=$LPHEX  arena=[$ASTART, +0x400000)"

AL="$OUT/arena.list"
echo "$ASTART 0x400000 arena" > "$AL"

# ---- 3) 等待到某个时刻（T0+offset 秒） ----
waittil() { # waittil <offset>
    TARGET=$((T0+$1))
    while :; do
        NOW=$(date +%s)
        [ "$NOW" -ge "$TARGET" ] && break
        REM=$((TARGET-NOW))
        [ $REM -gt 20 ] && sleep 20 || sleep 2
    done
}

# ---- 4) arena 时序列 A（对局 ~2:00 施法窗）----
waittil 70
say "[3] arena-A 时序列 12帧×3s  $(date '+%H:%M:%S')"
sh /data/adb/JAY/King_shot.sh "$AL" "$OUT/arenaA" 12 3 >>"$LOG" 2>&1
SZA=$(wc -c < "$OUT/arenaA/f01_arena.bin" 2>/dev/null)
if [ "$SZA" = "4194304" ] 2>/dev/null; then
    say "[3] arena-A 完成 $(date '+%H:%M:%S') (f01=4194304B OK)"
else
    say "[3] arena-A 完成 $(date '+%H:%M:%S') [!] f01大小异常=${SZA:-缺失}"
    FAILED=1
fi

# ---- 5) arena 时序列 B（对局 ~4:00 施法窗）----
waittil 195
say "[4] arena-B 时序列 12帧×3s  $(date '+%H:%M:%S')"
sh /data/adb/JAY/King_shot.sh "$AL" "$OUT/arenaB" 12 3 >>"$LOG" 2>&1
SZB=$(wc -c < "$OUT/arenaB/f01_arena.bin" 2>/dev/null)
if [ "$SZB" = "4194304" ] 2>/dev/null; then
    say "[4] arena-B 完成 $(date '+%H:%M:%S') (f01=4194304B OK)"
else
    say "[4] arena-B 完成 $(date '+%H:%M:%S') [!] f01大小异常=${SZB:-缺失}"
    FAILED=1
fi

# ---- 6) dump 1（对局 ~5:00, 兵线在场上）----
waittil 275
say "[5] dump1 开始 $(date '+%H:%M:%S')"
KEXEC dumpgame "$OUT/game1.bin" "$OUT/game1.idx" >"$OUT/dump1.log" 2>&1
say "[5] dump1 结束 $(date '+%H:%M:%S')"
tail -2 "$OUT/dump1.log" >>"$LOG" 2>/dev/null
grep -q '读失败' "$OUT/dump1.log" 2>/dev/null && say "[!] dump1 有读失败(已填零), 见 dump1.log"

# ---- 7) dump 2（对局 ~10:40, 差分找时钟）----
waittil 615
say "[6] dump2 开始 $(date '+%H:%M:%S')"
KEXEC dumpgame "$OUT/game2.bin" "$OUT/game2.idx" >"$OUT/dump2.log" 2>&1
say "[6] dump2 结束 $(date '+%H:%M:%S')"
tail -2 "$OUT/dump2.log" >>"$LOG" 2>/dev/null
grep -q '读失败' "$OUT/dump2.log" 2>/dev/null && say "[!] dump2 有读失败(已填零), 见 dump2.log"

# ---- 8) 收尾 ----
say ""
say "产物目录: $OUT"
ls -la "$OUT" >>"$LOG" 2>&1
if [ "$FAILED" = "0" ]; then
    say "[完成] collect 全部成功  $(date '+%H:%M:%S')"
    say "请通知 PC 端拉取: $OUT"
    exit 0
else
    say "[!] collect 结束但有异常(见上, 影响 CD 实验)  $(date '+%H:%M:%S')"
    say "请通知 PC 端拉取: $OUT"
    exit 3
fi
