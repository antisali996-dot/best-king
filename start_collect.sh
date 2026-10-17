#!/system/bin/sh
# ============================================================================
# start_collect.sh — 一键启动对局采集（MT 里"点击/运行"即可, 无需输入命令）
# ----------------------------------------------------------------------------
# 行为:
#   1) 检查 root / 依赖
#   2) 已在采集 -> 显示进度, 不重复启动
#   3) 未在采集 -> 把 collect.sh 转入后台运行, 并显示"怎么查看进度"
# 用法: MT 中点击本文件运行（或以 root 执行: sh /data/adb/JAY/start_collect.sh）
# ============================================================================

COLLECT=/data/adb/JAY/collect.sh

echo "=================================================="
echo "  S46 一键采集启动器"
echo "=================================================="

# ---- root 检查 ----
if ! id 2>/dev/null | grep -q 'uid=0'; then
    echo "[!] 当前不是 root 身份"
    echo "    请在 MT 里以 root 运行本脚本（长按文件→打开方式→终端/脚本执行，勾选 root）"
    exit 2
fi

# ---- 依赖检查 ----
if [ ! -f "$COLLECT" ]; then
    echo "[X] 缺少 $COLLECT — 请先把 collect.sh 推送到 /data/adb/JAY/"
    exit 2
fi

# ---- 已在运行? 不重复启动 ----
RUNNING=$(cat /proc/*/cmdline 2>/dev/null | tr '\0' ' ' | grep -c 'JAY/collect\.sh')
if [ "$RUNNING" -gt 0 ] 2>/dev/null; then
    echo "[i] 采集已在运行中, 不重复启动"
    LAST=$(ls -t /data/adb/JAY/auto/*/collect.log 2>/dev/null | head -1)
    if [ -n "$LAST" ]; then
        echo "    当前进度 ($LAST):"
        tail -6 "$LAST"
    fi
    echo "=================================================="
    exit 0
fi

# ---- 启动（后台运行, 关终端/切应用都不影响） ----
BEFORE=$(ls -t /data/adb/JAY/auto/*/collect.log 2>/dev/null | head -1)
setsid nohup sh "$COLLECT" >/dev/null 2>&1 &

# ---- 确认 + 使用说明（最多重试3次, 防止负载高时误报失败） ----
LAST=""
RUN2="0"
i=0
while [ $i -lt 3 ]; do
    sleep 2
    LAST=$(ls -t /data/adb/JAY/auto/*/collect.log 2>/dev/null | head -1)
    RUN2=$(cat /proc/*/cmdline 2>/dev/null | tr '\0' ' ' | grep -c 'JAY/collect\.sh')
    if [ -n "$LAST" ] && [ "$LAST" != "$BEFORE" ] && [ "$RUN2" -gt 0 ] 2>/dev/null; then
        break
    fi
    i=$((i+1))
done
if [ -n "$LAST" ] && [ "$LAST" != "$BEFORE" ] && [ "$RUN2" -gt 0 ] 2>/dev/null; then
    echo "[OK] 采集已启动（后台运行, 不用管它）"
    echo "--------------------------------------------------"
    echo "  你的动作只有两次:"
    echo "    对局时间 2:00 -> 1技能, 隔几秒 2技能, 隔几秒 大招"
    echo "    对局时间 4:00 -> 再来一轮（同上）"
    echo "--------------------------------------------------"
    echo "  看进度: 查看下面这个文件的最后几行:"
    echo "    $LAST"
    echo "  或在本目录再点一次本脚本, 会直接显示当前进度"
    echo "  要中止采集: 告诉助手即可"
    echo "=================================================="
    exit 0
else
    echo "[!] 启动后未能确认日志, 可能失败 — 把本页面截图发给助手"
    if [ -n "$LAST" ]; then
        echo "    最近日志: $LAST"
        tail -4 "$LAST"
    fi
    echo "=================================================="
    exit 3
fi
