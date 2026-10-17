#!/system/bin/sh
# ============================================================================
# stop_collect.sh — 一键停止采集（MT 里点击运行即可）
# ----------------------------------------------------------------------------
# 杀掉 collect.sh 及其派生的采集进程（King_shot / King_snap / King.sh dumpgame），
# 并显示最近一次采集日志的尾部，便于判断异常原因。
# 退出码: 0=已全部停止 2=环境问题(无root/无pkill) 3=仍有残留进程
# ============================================================================

echo "=================================================="
echo "  停止 S46 采集"
echo "=================================================="

# ---- root 检查 ----
if ! id 2>/dev/null | grep -q 'uid=0'; then
    echo "[!] 当前不是 root — 请在 MT 里以 root 运行"
    exit 2
fi

# ---- pkill 定位（pkill -> magisk busybox -> system busybox） ----
PK=""
PKSUB=""
if command -v pkill >/dev/null 2>&1; then
    PK="pkill"
elif [ -x /data/adb/magisk/busybox ]; then
    PK="/data/adb/magisk/busybox"; PKSUB="pkill"
elif [ -x /system/bin/busybox ]; then
    PK="/system/bin/busybox"; PKSUB="pkill"
else
    echo "[!] 未找到 pkill, 无法停止采集（请截图发助手）"
    exit 2
fi
kp() { # kp [信号] <模式...>
    if [ -n "$PKSUB" ]; then "$PK" "$PKSUB" "$@"
    else "$PK" "$@"; fi
}

# ---- 统计: 4 类采集进程的并集（避免只看 collect.sh 漏掉孤儿） ----
count_all() {
    cat /proc/*/cmdline 2>/dev/null | tr '\0' '\n' \
        | grep -cE 'JAY/(collect\.sh|King_shot\.sh|King_snap\.sh|King\.sh)'
}

N=$(count_all)
echo "[i] 当前采集进程数: $N"

# ---- 先礼 ----
kp -f 'JAY/collect\.sh' 2>/dev/null
kp -f 'JAY/King_shot\.sh' 2>/dev/null
kp -f 'JAY/King_snap\.sh' 2>/dev/null
kp -f 'JAY/King\.sh' 2>/dev/null
sleep 1

N2=$(count_all)
if [ "$N2" -gt 0 ] 2>/dev/null; then
    echo "[!] 仍有 $N2 个, 强制结束..."
    kp -9 -f 'JAY/collect\.sh' 2>/dev/null
    kp -9 -f 'JAY/King_shot\.sh' 2>/dev/null
    kp -9 -f 'JAY/King_snap\.sh' 2>/dev/null
    kp -9 -f 'JAY/King\.sh' 2>/dev/null
    sleep 1
    N2=$(count_all)
fi

if [ "$N2" -eq 0 ] 2>/dev/null; then
    echo "[OK] 采集已全部停止"
else
    echo "[!] 还剩 ${N2:-?} 个采集进程 — 请截图发助手"
fi

# ---- 显示最近日志尾部（帮助定位异常） ----
LAST=$(ls -t /data/adb/JAY/auto/*/collect.log 2>/dev/null | head -1)
if [ -n "$LAST" ]; then
    echo "--------------------------------------------------"
    echo "最近一次采集日志尾部:"
    echo "  $LAST"
    echo "--------------------------------------------------"
    tail -8 "$LAST" 2>/dev/null
fi

echo "=================================================="
echo "  停止完成"
echo "=================================================="

if [ "$N2" -eq 0 ] 2>/dev/null; then
    exit 0
else
    exit 3
fi
