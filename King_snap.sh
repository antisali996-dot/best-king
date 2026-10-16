#!/system/bin/sh
# ============================================================================
# King_snap.sh — 一键英雄数据快照（单脚本 → 单日志）
# ----------------------------------------------------------------------------
# 进对局后执行；自动定位 S46 英雄链并采样 N 帧：
#   · 每帧：10 英雄 uid/id/camp/slot/坐标（文本行，可读）
#   · 每帧：本地英雄 P(全量)/X1/X2/T/A/C1..C4 原始十六进制（离线差分找血量用）
# 产物: <输出目录>/live_<stamp>_<pid>.log —— 抓完只需要发回这一个文件
# 用法: sh King_snap.sh [帧数=8] [帧间隔秒=3] [输出目录=/data/adb/JAY/live]
# 退出码: 0=成功 2=参数/环境问题 3=链路定位失败(多为未进对局) 4=King.sh未找到
#         (信号中止: INT/HUP=130, TERM=143)
# 注意: Android /system/bin/sh = mksh, 其 $(( )) 为 32 位算术;
#       本脚本所有 48 位地址运算/格式转换全部经 awk(double, 2^53 内精确) 完成。
# ============================================================================

set -f    # 关闭路径名展开(保护 $AWK 双词形式不被 glob 干扰; 全脚本不依赖通配)

FRAMES=${1:-8}
INTS=${2:-3}
case "$FRAMES" in ''|*[!0-9]*) FRAMES=8;; esac
[ "$FRAMES" -ge 1 ] 2>/dev/null || FRAMES=8
case "$INTS" in ''|*[!0-9]*) INTS=3;; esac
[ "$INTS" -ge 0 ] 2>/dev/null || INTS=3

# ---- 工具自检 + awk 定位(必须支持大整数输出) ----
for t in od dd head grep printf wc rm mkdir sleep date dirname; do
    command -v "$t" >/dev/null 2>&1 || { echo "[X] 缺少工具: $t"; exit 2; }
done
AWK=""
awk_ok() {
    [ "$("$@" 'BEGIN{print 4294967296+1}' 2>/dev/null)" = "4294967297" ] || return 1
    [ "$("$@" -v h=7 'BEGIN{n=split("1F",c,""); print n+h}' 2>/dev/null)" = "9" ] || return 1
    return 0
}
if awk_ok awk; then AWK="awk"
elif awk_ok /data/adb/magisk/busybox awk; then AWK="/data/adb/magisk/busybox awk"
elif awk_ok /system/bin/busybox awk; then AWK="/system/bin/busybox awk"
elif awk_ok /system/xbin/busybox awk; then AWK="/system/xbin/busybox awk"
else
    echo "[X] 未找到支持大整数输出的 awk (试过 awk / magisk-busybox / system-busybox)"
    exit 2
fi

# ---- 定位 King.sh ----
K=""
SELFDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
for cand in "$SELFDIR/King.sh" /data/adb/JAY/King.sh /data/local/tmp/King.sh; do
    if [ -f "$cand" ]; then K=$cand; break; fi
done
if [ -z "$K" ]; then
    echo "[X] 未找到 King.sh 主程序"
    exit 4
fi
IS_ELF=0
head -c 4 "$K" 2>/dev/null | grep -q "$(printf '\177ELF')" 2>/dev/null && IS_ELF=1

OUT=${3:-/data/adb/JAY/live}
mkdir -p "$OUT" 2>/dev/null || { echo "[X] 无法创建输出目录: $OUT"; exit 2; }
TMP="$OUT/.klv.$$"
rm -rf "$TMP"
mkdir -p "$TMP" 2>/dev/null || { echo "[X] 无法创建临时目录: $TMP"; exit 2; }
trap 'rm -rf "$TMP"' EXIT
trap 'rm -rf "$TMP"; exit 130' INT HUP
trap 'rm -rf "$TMP"; exit 143' TERM
STAMP=$(date '+%y%m%d_%H%M%S' 2>/dev/null)
[ -z "$STAMP" ] && STAMP=$$
LOG="$OUT/live_${STAMP}_$$.log"
if ! : > "$LOG" 2>/dev/null; then
    echo "[X] 无法写日志: $LOG"
    exit 2
fi

say() { echo "$@" >> "$LOG"; echo "$@"; }

# ======================== 数值工具 (规避 mksh 32 位算术) ========================
h2d() {  # 16进制字符串(可带0x) -> 十进制 (必须以 print 输出, 保证 %lld 全精度)
    $AWK -v h="$1" 'BEGIN{
        h=toupper(h); sub(/^0X/,"",h); v=0; n=split(h,c,"");
        if(n<1){print "ERR"; exit}
        for(i=1;i<=n;i++){d=index("0123456789ABCDEF",c[i])-1; if(d<0){print "ERR"; exit}; v=v*16+d}
        print v
    }'
}
d2h() {  # 十进制字符串 -> 0x16进制 (纯字符串构造, 不用 printf)
    $AWK -v v="$1" 'BEGIN{
        n=v+0; if(n<0){print "ERR"; exit}
        if(n==0){print "0x0"; exit}
        h=""; while(n>0){d=n%16; h=substr("0123456789ABCDEF",d+1,1) h; n=int((n-d)/16)}
        print "0x" h
    }'
}
aadd() { # 十进制 + 十进制 (48 位内精确; 必须以 print 输出)
    $AWK -v a="$1" -v b="$2" 'BEGIN{print a+b}'
}
inrg() { # inrg <dec> <decLo> <decHi>  -> 0/1
    $AWK -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN{exit !(v>=lo && v<=hi)}'
}

# ======================== 读取/解析工具 ========================
run_raw() {   # run_raw <stdoutfile> <args...>
    O=$1; shift
    if [ "$IS_ELF" = 1 ]; then
        "$K" "$@" </dev/null >"$O" 2>/dev/null
    else
        sh "$K" "$@" </dev/null >"$O" 2>/dev/null
    fi
}
kread() {   # kread <decAddr> <decLen> <outfile>; 失败=非0, 保证无脏数据残留
    case "$1" in ''|*[!0-9]*) say "[X] kread 非法地址: $1"; return 1;; esac
    case "$2" in ''|*[!0-9]*) say "[X] kread 非法长度: $2"; return 1;; esac
    rm -f "$3"
    run_raw /dev/null range "$(d2h "$1")" "$(d2h "$2")" "$3" || return 1
    [ -s "$3" ] || return 1
    return 0
}
fptr() { o=$(($2)); dd if="$1" bs=1 skip="$o" count=6 2>/dev/null | od -An -tu2 | $AWK '{print $1+$2*65536+$3*4294967296}'; }
fu32() { o=$(($2)); dd if="$1" bs=1 skip="$o" count=4 2>/dev/null | od -An -tu4 | $AWK '{print $1}'; }
fi32() { o=$(($2)); dd if="$1" bs=1 skip="$o" count=4 2>/dev/null | od -An -td4 | $AWK '{print $1}'; }
hexp() { d2h "$1"; }

PTR_LO=481036337152      # 0x7000000000
PTR_HI=549755813887      # 0x7FFFFFFFFF

say "=================================================="
say "King_snap · $(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)"
say "King.sh=$K (ELF=$IS_ELF)   帧数=$FRAMES  间隔=${INTS}s"
say "=================================================="

# ---- [1] libGameCore base / bss ----
run_raw "$TMP/maps.txt" maps libGameCore
BASEHEX=$($AWK '/libGameCore/ && $3=="00000000" {split($1,a,"-"); print a[1]; exit}' "$TMP/maps.txt")
if [ -z "$BASEHEX" ]; then
    say "[X] maps 未取到 libGameCore 基址 (驱动/进程问题?)"
    exit 3
fi
BASE=$(h2d "$BASEHEX")
if [ "$BASE" = "ERR" ]; then say "[X] base 解析失败: $BASEHEX"; exit 3; fi
BSS=$(aadd "$BASE" 86437888)          # +0x526F000
say "[1] libGameCore base=$(hexp $BASE)  bss=$(hexp $BSS)"

# ---- [2] root / L1 / recarray ----
kread "$(aadd "$BSS" 10160)" 8 "$TMP/slot.bin" || { say "[X] 驱动读失败(slot)"; exit 3; }   # +0x27B0
ROOT=$(fptr "$TMP/slot.bin" 0)
if ! inrg "$ROOT" $PTR_LO $PTR_HI; then
    say "[X] root 无效($ROOT) — 多为未进对局或页隐藏"; exit 3
fi
kread "$ROOT" 6 "$TMP/root.bin" || { say "[X] 驱动读失败(root)"; exit 3; }
RC=$(fptr "$TMP/root.bin" 0)
SC="OK"; [ "$RC" != "$ROOT" ] && SC="WARN(self=$RC)"
say "[2] root=$(hexp $ROOT)  自指针=$SC"

kread "$(aadd "$ROOT" 976)" 8 "$TMP/l1.bin" || { say "[X] 驱动读失败(L1)"; exit 3; }          # +0x3D0
L1=$(fptr "$TMP/l1.bin" 0)
if ! inrg "$L1" $PTR_LO $PTR_HI; then say "[X] L1 无效"; exit 3; fi
kread "$(aadd "$L1" 56)" 40 "$TMP/l1h.bin" || { say "[X] 驱动读失败(L1hdr)"; exit 3; }       # +0x38 len 0x28
N58=$(fu32 "$TMP/l1h.bin" 0x20)
N5C=$(fu32 "$TMP/l1h.bin" 0x24)
if [ "$N58" != "16" ] || [ "$N5C" != "10" ]; then
    say "[X] L1 签名不符 (+0x58=$N58 +0x5C=$N5C)"; exit 3
fi
kread "$(aadd "$L1" 64)" 8 "$TMP/rec.bin" || { say "[X] 驱动读失败(rec)"; exit 3; }           # +0x40
REC=$(fptr "$TMP/rec.bin" 0)
say "[3] L1=$(hexp $L1) 签名OK  recarray=$(hexp $REC)"

# ---- [3] 十英雄表 ----
kread "$REC" 320 "$TMP/rows.bin" || { say "[X] rows 读失败"; exit 3; }                       # 0x140
say "[4] 英雄表:"
say "  idx uid   id  camp slot  P              X1             X        Z"
LOCALIDX=""
i=0
while [ "$i" -le 9 ]; do
    ro=$((i * 0x20))
    HUID=$(fu32 "$TMP/rows.bin" $ro)
    W=$(fptr "$TMP/rows.bin" $((ro + 8)))
    if ! inrg "$W" $PTR_LO $PTR_HI; then
        say "  [$i] wrapper 无效 ($W)"; i=$((i+1)); continue
    fi
    if ! kread "$W" 224 "$TMP/w.bin"; then
        say "  [$i] wrapper 读失败"; i=$((i+1)); continue
    fi
    WUID=$(fu32 "$TMP/w.bin" 0x14)
    P=$(fptr "$TMP/w.bin" 0xD8)
    if ! inrg "$P" $PTR_LO $PTR_HI; then
        say "  [$i] uid=$HUID P 无效"; i=$((i+1)); continue
    fi
    if ! kread "$P" 544 "$TMP/p.bin"; then
        say "  [$i] uid=$HUID P 读失败"; i=$((i+1)); continue
    fi
    ID=$(fu32 "$TMP/p.bin" 0x28)
    CAMP=$(fu32 "$TMP/p.bin" 0x34)
    PUID=$(fu32 "$TMP/p.bin" 0x3C)
    SLOT=$(fu32 "$TMP/p.bin" 0xB0)
    X1=$(fptr "$TMP/p.bin" 0x140)
    X2=$(fptr "$TMP/p.bin" 0x158)
    if [ -n "$X1" ] && inrg "$X1" $PTR_LO $PTR_HI && kread "$(aadd "$X1" 12480)" 8 "$TMP/x1p.bin"; then
        PX=$(fi32 "$TMP/x1p.bin" 0)
        PZ=$(fi32 "$TMP/x1p.bin" 4)
    else
        PX="ERR"; PZ="ERR"
    fi
    eval "UID_$i=$HUID; ID_$i=$ID; CAMP_$i=$CAMP; SLOT_$i=$SLOT; X1_$i=$X1; X2_$i=$X2; P_$i=$P"
    if [ -z "$LOCALIDX" ] && [ "$HUID" -lt 6000 ] 2>/dev/null; then LOCALIDX=$i; fi
    WARN=""
    [ "$WUID" != "$HUID" ] && WARN=" uidW=$WUID"
    [ "$PUID" != "$HUID" ] && WARN="$WARN uidP=$PUID"
    say "  [$i] $HUID $ID $CAMP $SLOT $(hexp $P) $(hexp $X1) $PX $PZ$WARN"
    i=$((i + 1))
done
if [ -z "$LOCALIDX" ]; then
    LOCALIDX=0
    say "[!] 未识别本地英雄(uid<6000), 回退 idx=0"
fi
eval "LP=\$P_$LOCALIDX; LX1=\$X1_$LOCALIDX; LX2=\$X2_$LOCALIDX; LID=\$ID_$LOCALIDX; LUID=\$UID_$LOCALIDX"
if [ -z "$LP" ] || ! inrg "$LP" $PTR_LO $PTR_HI; then
    say "[X] 本地英雄 P 无效, 终止 (链路半通)"
    exit 3
fi
kread "$LP" 544 "$TMP/ploc.bin" || { say "[X] 本地 P 读失败"; exit 3; }
LT=$(fptr "$TMP/ploc.bin" 0x188)
LA=$(fptr "$TMP/ploc.bin" 0xa8)
LC1=$(fptr "$TMP/ploc.bin" 0x1d0)
LC2=$(fptr "$TMP/ploc.bin" 0x1e8)
LC3=$(fptr "$TMP/ploc.bin" 0x200)
LC4=$(fptr "$TMP/ploc.bin" 0x218)
say "[5] 本地英雄: idx=$LOCALIDX uid=$LUID id=$LID P=$(hexp $LP)"

# ---- [4] 多帧采样 ----
F=1
while [ "$F" -le "$FRAMES" ]; do
    say ""
    say "===== FRAME $F/$FRAMES  $(date '+%H:%M:%S' 2>/dev/null) ====="
    i=0
    while [ "$i" -le 9 ]; do
        eval "U=\$UID_$i; ID=\$ID_$i; C=\$CAMP_$i; X1=\$X1_$i"
        if [ -n "$X1" ] && inrg "$X1" $PTR_LO $PTR_HI; then
            if kread "$(aadd "$X1" 12480)" 8 "$TMP/x1p.bin"; then
                PX=$(fi32 "$TMP/x1p.bin" 0)
                PZ=$(fi32 "$TMP/x1p.bin" 4)
                say "POS $i $U ${ID:--1} ${C:--1} $PX $PZ"
            else
                say "POS $i $U ${ID:--1} ${C:--1} ERR ERR"
            fi
        fi
        i=$((i + 1))
    done
    # 本地十六进制块（供离线差分; 名称: P/X1/X2/T/A/C1..C4）
    if kread "$LP" 6280 "$TMP/hexP.bin"; then
        say "HEX P $(hexp $LP) 0x1888"
        od -An -v -tx4 "$TMP/hexP.bin" >> "$LOG"
    else
        say "HEX P $(hexp $LP) 0x1888 FAIL"
    fi
    if [ -z "$LX1" ] || ! inrg "$LX1" $PTR_LO $PTR_HI; then
        say "HEX X1 SKIP"
    elif kread "$LX1" 12800 "$TMP/hexX1.bin"; then
        say "HEX X1 $(hexp $LX1) 0x3200"
        od -An -v -tx4 "$TMP/hexX1.bin" >> "$LOG"
    else
        say "HEX X1 $(hexp $LX1) 0x3200 FAIL"
    fi
    if [ -z "$LX2" ] || ! inrg "$LX2" $PTR_LO $PTR_HI; then
        say "HEX X2 SKIP"
    elif kread "$LX2" 1024 "$TMP/hexX2.bin"; then
        say "HEX X2 $(hexp $LX2) 0x400"
        od -An -v -tx4 "$TMP/hexX2.bin" >> "$LOG"
    else
        say "HEX X2 $(hexp $LX2) 0x400 FAIL"
    fi
    o=0
    for t in "$LT" "$LA" "$LC1" "$LC2" "$LC3" "$LC4"; do
        o=$((o + 1))
        case "$o" in 1) NM=T;; 2) NM=A;; 3) NM=C1;; 4) NM=C2;; 5) NM=C3;; 6) NM=C4;; esac
        if [ -z "$t" ] || ! inrg "$t" $PTR_LO $PTR_HI; then say "HEX $NM SKIP"; continue; fi
        case "$o" in 3|4) CLN=8192; CHEX=0x2000;; *) CLN=2048; CHEX=0x800;; esac
        if kread "$t" "$CLN" "$TMP/hexC.bin"; then
            say "HEX $NM $(hexp $t) $CHEX"
            od -An -v -tx4 "$TMP/hexC.bin" >> "$LOG"
        else
            say "HEX $NM $(hexp $t) $CHEX FAIL"
        fi
    done
    F=$((F + 1))
    if [ "$F" -le "$FRAMES" ] && [ "$INTS" -gt 0 ]; then
        sleep "$INTS"
    fi
done

say ""
say "[完成] 日志: $LOG  ($(wc -c "$LOG" | $AWK '{print $1}') 字节)"
exit 0
