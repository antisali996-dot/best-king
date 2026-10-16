#!/system/bin/sh
# ============================================================================
# King_battle.sh — best-king 战局一键 dump 包装 (v1.0)
# ----------------------------------------------------------------------------
# 用途: 对局内(开局60秒后)执行, 生成独立会话目录, 供离线收网(p34 脚本)使用。
# 产物: /data/adb/JAY/battle/<mmina_hhmm>/
#       ├─ game.bin        (主内存 dump)
#       ├─ game.idx        (分段索引, 含完整 maps)
#       └─ libGameCore.so  (进程内映射的库本体, 供静态xref锚点用)
# 用法:
#   su
#   sh /data/adb/JAY/King_battle.sh          # 立刻 dump (请已在战中)
#   sh /data/adb/JAY/King_battle.sh 90       # 先等 90 秒再 dump
# 前提: 同目录(或 /data/adb/JAY)下有 best-king 的 King.sh 主程序(ELF)。
# 退出码: 0=成功 4=King.sh未找到 5=进程未在运行 6=dump产物缺失 7=磁盘空间不足
# ============================================================================

BTDIR=/data/adb/JAY
OUTROOT=$BTDIR/battle
WK=60            # 默认延时(秒), 可被第1参数覆盖
PKG=com.tencent.tmgp.sgame

# ---- 参数: 等待秒数 ----
if [ -n "$1" ] && [ "$1" -ge 0 ] 2>/dev/null; then
    WK=$1
fi

echo "=================================================="
echo "  best-king · King_battle.sh  (战中 dump, 延时 ${WK}s)"
echo "=================================================="

# ---- [1] 定位 King.sh 主程序 ----
SELFDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
KING_BIN=""
for cand in "$SELFDIR/King.sh" "$BTDIR/King.sh" /data/local/tmp/King.sh /sdcard/King.sh; do
    if [ -f "$cand" ]; then KING_BIN=$cand; break; fi
done
if [ -z "$KING_BIN" ]; then
    echo "[X] 未找到 King.sh 主程序 (放在 $BTDIR/King.sh 或与本脚本同目录)"
    exit 4
fi
if [ "$(id -u 2>/dev/null)" != "0" ]; then
    echo "[!] 提示: 当前非 root, 让 MT/终端以 root 运行本脚本"
fi

# ---- [2] 目标进程检查 ----
PID=$(pidof "$PKG" 2>/dev/null)
if [ -z "$PID" ]; then
    echo "[X] $PKG 未在运行"
    exit 5
fi
echo "[1/4] 目标进程: pid=$PID"

# ---- [3] 磁盘空间检查(至少 4GB) ----
AVAIL_KB=$(df -k /data 2>/dev/null | tail -1 | awk '{print $4}')
if [ -n "$AVAIL_KB" ] && [ "$AVAIL_KB" -lt 4194304 ] 2>/dev/null; then
    echo "[X] /data 可用 ${AVAIL_KB}KB < 4GB, 先清理空间"
    exit 7
fi

# ---- [4] 倒计时 ----
if [ "$WK" -gt 0 ] 2>/dev/null; then
    echo "[2/4] 等待 ${WK} 秒 (请确认已在对局内 ~60s+)"
    n=$WK
    while [ $n -gt 0 ]; do
        echo "  ${n}s ..."; sleep 5; n=$((n-5))
    done
fi

# ---- 输出目录 ----
STAMP=$(date +%m%d_%H%M 2>/dev/null)
[ -z "$STAMP" ] && STAMP=$$
OUT=$OUTROOT/$STAMP
mkdir -p "$OUT" 2>/dev/null || { echo "[X] 无法创建 $OUT"; exit 6; }
echo "[3/4] 会话目录: $OUT"

# ---- dumpgame (主.dump) ----
IS_ELF=0
if head -c 4 "$KING_BIN" 2>/dev/null | grep -q "$(printf '\177ELF')" 2>/dev/null; then
    IS_ELF=1
fi
if [ "$IS_ELF" = 1 ]; then
    echo "[*] King.sh = ELF 主程序, 直接执行"
    "$KING_BIN" dumpgame "$OUT/game.bin" "$OUT/game.idx" && RC_DUMP=0 || RC_DUMP=$?
else
    echo "[*] King.sh = 文本脚本, 用 sh 执行"
    sh "$KING_BIN" dumpgame "$OUT/game.bin" "$OUT/game.idx" && RC_DUMP=0 || RC_DUMP=$?
fi
if [ "$RC_DUMP" != "0" ] 2>/dev/null; then
    echo "[!] King.sh dumpgame 返回 rc=$RC_DUMP(产物可能不完整)"
    RC=6
fi

# ---- 抓 libGameCore.so (进程内映射路径) ----
LIBPATH=$(cat /proc/$PID/maps 2>/dev/null | grep 'libGameCore.so' | head -1 | awk '{print $6}')
if [ -n "$LIBPATH" ]; then
    echo "[*] libGameCore.so 路径: $LIBPATH"
    cp "$LIBPATH" "$OUT/libGameCore.so" 2>/dev/null \
      || cat "$LIBPATH" > "$OUT/libGameCore.so" 2>/dev/null
fi

# ---- 产物校验 ----
echo "[4/4] 产物校验:"
RC=0
for f in game.bin game.idx; do
    if [ -s "$OUT/$f" ]; then
        SZ=$(du -k "$OUT/$f" 2>/dev/null | awk '{print $1}')
        echo "  [OK] $OUT/$f (${SZ}KB)"
    else
        echo "  [X] 缺 $OUT/$f"; RC=6
    fi
done
if [ -s "$OUT/libGameCore.so" ]; then
    echo "  [OK] $OUT/libGameCore.so"
else
    echo "  [!] libGameCore.so 抓取失败 (不影响离线分析, 但静态锚点少一份)"
fi

if [ "$RC" = 0 ]; then
    echo ""
    echo "[完成] 把以下两个文件发回 PC (jobs 用 adb):"
    echo "  $OUT/game.bin"
    echo "  $OUT/game.idx"
    echo "PC 端拉取示例(powershell):"
    echo "  adb exec-out su -c 'cat $OUT/game.bin' | Set-Content -Encoding Byte D:\\OPENCODE\\tmp\\autodump\\battle\\game.bin"
    echo "  adb exec-out su -c 'cat $OUT/game.idx' | Set-Content -Encoding Byte D:\\OPENCODE\\tmp\\autodump\\battle\\game.idx"
fi
exit $RC
