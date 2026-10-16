#!/system/bin/sh
# sweep12.sh: 12 twins × 全域 = 12 次 bpfollow-style 16-pt 扫描
# 快速连发, 一次覆盖三个英雄 object 全域骨骼
K=/data/adb/JAY/King.sh
$K bpsweep 0x75e79b7cc8 0x15A0 300 2>&1 | tail -30
$K bpsweep 0x75e79b6920 0x15A0 300 2>&1 | tail -30
$K bpsweep 0x75e79b5578 0x15A0 300 2>&1 | tail -30
$K bpsweep 0x75e79b41d0 0x15A0 300 2>&1 | tail -30
