#!/system/bin/sh
# ============================================================================
# King_bpauto.sh — 战局字段观察点一键 (Plan B)
# ----------------------------------------------------------------------------
# 用途: 对局战斗中执行, 自动: 取真.bss → 自证 root → L1 → recarray → 当前局
#       wrapper[0] → 挂 16 点硬件观察点(0x130..0x2C8) → 采样 → 打印写入者现场。
# 用法:
#   su
#   sh /data/adb/JAY/King_bpauto.sh            # 默认采样 20s
#   sh /data/adb/JAY/King_bpauto.sh 30000      # 采样 30s
#   sh /data/adb/JAY/King_bpauto.sh 20000 2    # m: 2=只测写 3=读写 4=执行
# 前提: /data/adb/JAY/King.sh 主程序(v1.3+) 与 par 驱动, 游戏正在战斗中。
# 输出: point[i] +0xXXX 命中数 + hit0..hitN (pc/lr/x0/x1/x2/x8/x19/x20)
#       → 原样发回 PC 解码即可。
# 退出码: 0=成功 4=未找到King.sh 5=游戏未运行 11=驱动阻断
# ============================================================================

POW=20000                        # 默认采样 20 秒
MOD=2                            # 默认 write-only
if [ -n "$1" ] && [ "$1" -ge 0 ] 2>/dev/null; then POW=$1; fi
if [ -n "$2" ] && [ "$2" -ge 0 ] 2>/dev/null; then MOD=$2; fi

SELFDIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
BT=/data/adb/JAY
for d in "$SELFDIR" "$BT" /data/local/tmp /sdcard; do
    if [ -f "$d/King.sh" ]; then K="$d/King.sh"; break; fi
done
if [ -z "$K" ]; then
    echo "[X] 未找到 King.sh 主程序 (放 $BT/King.sh)"
    exit 4
fi
if ! pgrep -f com.tencent.tmgp.sgame >/dev/null 2>&1; then
    echo "[X] 王者不在运行 (pgrep 无 tmgp)"
    exit 5
fi

# ELF 直接执行; 文本才挂 sh
IS_ELF=0
if head -c 4 "$K" 2>/dev/null | grep -q "$(printf '\177ELF')" 2>/dev/null; then IS_ELF=1; fi

echo "== King_bpauto.sh  (采样 ${POW}ms, mode=${MOD}) =="
if [ "$IS_ELF" = 1 ]; then
    exec "$K" bpauto "$POW" "$MOD"
else
    exec sh "$K" bpauto "$POW" "$MOD"
fi
