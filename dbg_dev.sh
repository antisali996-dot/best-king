#!/system/bin/sh
# dbg_dev.sh — 看有哪些驱动 device + 确认 hwbp 用啥 device 文件
ls /dev/NVISPI /dev/aputag /dev/asound /dev/clkdbg /dev/crypto /dev/kpm /dev/paradise /dev/linyu 2>/dev/null
echo ---
ls /dev/ | grep -Ev '^(input|block|mdmg|mdio|ptp|pps|snd|timer|bus|cpu|gpio|fuse|dri)' | head -40
