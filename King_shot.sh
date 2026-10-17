#!/system/bin/sh
# ============================================================================
# King_shot.sh — best-king 批量定向抓帧 (一帧 = 清单中所有区间的一次集合抓取)
# ----------------------------------------------------------------------------
# 用途: 走位验证 / 受伤差分等"多帧小快照"场景。避免多次 adb 往返与手工敲长命令。
# 用法:
#   sh King_shot.sh <清单文件> <输出目录> [帧数=1] [帧间隔秒=0]
# 清单格式(每行): <hex起址> <hex长度> <名字>     # 空行/# 开头忽略
# 产物: <输出目录>/f01_<名字>.bin, f02_...
# 退出码: 0=全成功 2=参数/清单问题 3=存在失败段 4=King.sh未找到
# 示例:
#   echo "0x758dca6000 0x2800 heroX2free" > /data/adb/JAY/shot.list
#   sh /data/adb/JAY/King_shot.sh /data/adb/JAY/shot.list /data/adb/JAY/shot 2 3
# ============================================================================
LIST=$1
OUT=$2
FRAMES=${3:-1}
INTS=${4:-0}
if [ -z "$LIST" ] || [ -z "$OUT" ]; then
    echo "用法: sh King_shot.sh <清单文件> <输出目录> [帧数=1] [帧间隔秒=0]"
    exit 2
fi
if [ ! -f "$LIST" ]; then
    echo "[X] 清单不存在: $LIST"
    exit 2
fi
case "$FRAMES" in ''|*[!0-9]*) FRAMES=1;; esac
[ "$FRAMES" -ge 1 ] 2>/dev/null || FRAMES=1
case "$INTS" in ''|*[!0-9]*) INTS=0;; esac

K=""
SELFDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
for cand in "$SELFDIR/King.sh" /data/adb/JAY/King.sh /data/local/tmp/King.sh /sdcard/King.sh; do
    if [ -f "$cand" ]; then K=$cand; break; fi
done
if [ -z "$K" ]; then
    echo "[X] 未找到 King.sh 主程序 (放在 /data/adb/JAY/King.sh 或与本脚本同目录)"
    exit 4
fi
IS_ELF=0
if head -c 4 "$K" 2>/dev/null | grep -q "$(printf '\177ELF')" 2>/dev/null; then
    IS_ELF=1
fi

mkdir -p "$OUT" 2>/dev/null || { echo "[X] 无法创建目录: $OUT"; exit 2; }

# 清单空检查 + 规范化(去 CR, 兼容 Windows 侧编辑过的清单)
if [ ! -s "$LIST" ]; then
    echo "[X] 清单为空: $LIST"
    exit 2
fi
TL="$OUT/.shot_list.norm"
tr -d '\r' < "$LIST" > "$TL" 2>/dev/null
[ -s "$TL" ] || { rm -f "$TL"; echo "[X] 清单读取/规范化失败: $LIST"; exit 2; }

LINES=0
while read -r A S N; do
    case "$A" in ""|"#"*) continue;; esac
    LINES=$((LINES+1))
done < "$TL"
if [ "$LINES" -lt 1 ]; then
    rm -f "$TL"
    echo "[X] 清单为空: $LIST"
    exit 2
fi

echo "=================================================="
echo "  best-king · King_shot.sh    ${LINES} 段 x ${FRAMES} 帧 (间隔 ${INTS}s)"
echo "  清单: $LIST"
echo "  输出: $OUT"
echo "=================================================="

run_range() {
    if [ "$IS_ELF" = 1 ]; then
        if [ -x /system/bin/linker64 ]; then
            /system/bin/linker64 "$K" range "$1" "$2" "$3" </dev/null >/dev/null 2>&1
        else
            "$K" range "$1" "$2" "$3" </dev/null >/dev/null 2>&1
        fi
    else
        sh "$K" range "$1" "$2" "$3" </dev/null >/dev/null 2>&1
    fi
}

F=1
FAILS=0
while [ "$F" -le "$FRAMES" ]; do
    IDX=0
    while read -r A S N; do
        case "$A" in ""|"#"*) continue;; esac
        IDX=$((IDX+1))
        OF=$(printf '%s/f%02d_%s.bin' "$OUT" "$F" "$N")
        if run_range "$A" "$S" "$OF"; then
            SZ=$(wc -c "$OF" 2>/dev/null | awk '{print $1}')
            echo "  [f$F $IDX/$LINES] $N  $A + $S  = ${SZ:-0}B  OK"
        else
            echo "  [f$F $IDX/$LINES] $N  $A + $S  FAIL  (复跑: $K range $A $S $OF)"
            FAILS=$((FAILS+1))
        fi
    done < "$TL"
    echo "[帧 $F/$FRAMES] 完成  $(date '+%H:%M:%S' 2>/dev/null)"
    F=$((F+1))
    if [ "$F" -le "$FRAMES" ] && [ "$INTS" -gt 0 ]; then
        sleep "$INTS"
    fi
done

rm -f "$TL"
echo "--------------------------------------------------"
if [ "$FAILS" -eq 0 ]; then
    echo "[OK] 全部成功: $((FRAMES*LINES)) 段 -> $OUT"
    exit 0
else
    echo "[!] 有失败段: $FAILS / $((FRAMES*LINES))（失败明细见上, 可手动复跑）"
    exit 3
fi
