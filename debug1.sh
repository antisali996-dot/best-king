#!/system/bin/sh
# debug1.sh — Dumper bpauto vs King bpauto 差异对照
echo "== Dumper bpauto =="
/data/adb/JAY/Dumper.sh bpauto 3000 2>&1 | tail -10
echo "== King  bpauto =="
/data/adb/JAY/King.sh  bpauto 3000 2>&1 | tail -10
echo "== 类内差异内容结束 =="
