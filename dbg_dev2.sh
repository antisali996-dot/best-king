#!/system/bin/sh
ls /dev/ > /data/adb/dev_list.txt 2>&1
grep -iE 'nvispi|aputag|asound|clkdbg|crypto|driver|paradise|linyu|kpm' /data/adb/dev_list.txt
