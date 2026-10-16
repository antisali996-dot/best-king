#!/system/bin/sh
# grab_body.sh: wrapper±0x400 全抓 + 目标块 0x800 各
SU() { "$@"; }
D=/data/adb/JAY/battle
H=/data/adb/JAY/King.sh
$H range 0x75e4a30a94    0x800 $D/b0.bin 2>&1 | tail -1
$H range 0x75e77d9738    0x800 $D/b1.bin 2>&1 | tail -1
$H range 0x75e79b78c8    0x1000 $D/wfull.bin 2>&1 | tail -1
