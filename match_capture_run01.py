# -*- coding: utf-8 -*-
# match_capture_run01.py — 验收包 run-01 采集（对局内运行）
#   顺序: run-info -> maps -> [站定等待] 投影(framesnap+截图) -> CD 采样 -> 全量快照(+read-status) -> metadata
#   用法: python match_capture_run01.py [cd_seconds=240]
#   产物: D:\OPENCODE\workspace\android-memory-research\external\validation\run-01\
#     run-info.txt / maps.txt / game.bin / game.idx / read-status.jsonl
#     shot_proj.png / framesnap_proj.json
#     shot_cd.png   / cd.jsonl
#     metadata/global-metadata.dat + metadata-source.txt
import subprocess, time, sys, os, re, json
from collections import defaultdict

ADB = r'D:\OPENCODE\workspace\xiaogu-kernel\tools\_vendor\platform-tools\adb.exe'
K = '/system/bin/linker64 /data/adb/JAY/King.sh'
OUT = r'D:\OPENCODE\workspace\android-memory-research\external\validation\run-01'
PKG = 'com.tencent.tmgp.sgame'
WARMUP = 25          # 投影前站定等待（给用户看提示的时间）


def sh(cmd, tries=6, timeout=900, raw=False):
    for _ in range(tries):
        try:
            args = [ADB] + ([('exec-out' if raw else 'shell'), 'su', '-c', 'nsenter -t 1 -m -- ' + cmd])
            p = subprocess.run(args, capture_output=True, timeout=timeout)
            if raw:
                if p.stdout:
                    return p.stdout
                continue   # 空输出重试
            out = p.stdout.decode('utf-8', errors='replace')
            if out.strip():
                return out
        except subprocess.TimeoutExpired:
            pass
        time.sleep(0.6)
    return b'' if raw else ''


def log(msg):
    line = '[%s] %s' % (time.strftime('%H:%M:%S'), msg)
    print(line, flush=True)
    with open(os.path.join(OUT, 'run-info.txt'), 'a', encoding='utf-8') as f:
        f.write(line + '\n')


def screenshot(name):
    b = sh('screencap -p', raw=True)
    if b[:8] == b'\x89PNG\r\n\x1a\n':
        open(os.path.join(OUT, name), 'wb').write(b)
        log('截图 %s (%d B)' % (name, len(b)))
        return True
    log('!! 截图失败 %s (%d B)' % (name, len(b)))
    return False


def main():
    dur = int(sys.argv[1]) if len(sys.argv) > 1 else 240
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, 'run-info.txt'), 'w', encoding='utf-8').close()
    t_start = time.time()
    log('=== run-01 采集开始 ===')
    # 1) run-info: 版本 / PID / 进程启动时间 / 屏幕
    info = sh('dumpsys package %s | grep -m2 versionName; pidof %s; wm size; wm density' % (PKG, PKG))
    log('设备信息:\n' + info.strip())
    pid = ''
    for line in info.splitlines():
        if line.strip().isdigit():
            pid = line.strip()
    if pid:
        st = sh('ls -ld --time-style=+%%Y-%%m-%%dT%%H:%%M:%%S /proc/%s' % pid)
        if not re.search(r'\d{4}-\d{2}-\d{2}T', st):
            # toybox ls 不支持 --time-style 时: /proc/<pid>/stat 第22字段 + /proc/stat btime
            pstat = sh('cat /proc/%s/stat' % pid)
            boot = sh('grep -m1 btime /proc/stat')
            try:
                ticks = int(pstat.split(') ')[1].split()[19])
                btime = int(boot.split()[1])
                st = 'epoch=%d (%s)' % (btime + ticks // 100,
                                        time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(btime + ticks // 100)))
            except Exception:
                st = st.strip() or 'unknown'
        log('PID=%s 进程启动: %s' % (pid, st.strip()))
    # 2) maps.txt
    mp = sh(K + ' maps')
    open(os.path.join(OUT, 'maps.txt'), 'w', encoding='utf-8').write(mp)
    log('maps.txt (%d 行)' % len(mp.splitlines()))
    # 3) 站定等待 -> 投影快照
    log('请站定不动：%d 秒后采投影（framesnap+截图）' % WARMUP)
    time.sleep(WARMUP)
    screenshot('shot_proj.png')
    sh(K + ' framesnap /data/adb/JAY/val/framesnap.json')
    fs = sh('cat /data/adb/JAY/val/framesnap.json')
    if fs.strip():
        open(os.path.join(OUT, 'framesnap_proj.json'), 'w', encoding='utf-8').write(fs)
        log('framesnap_proj.json (%d B): %s' % (len(fs), fs.strip()[:160]))
    else:
        log('!! framesnap 输出为空（链未就绪或不在对局）')
    screenshot('shot_proj2.png')
    # 4) CD 三段采样（释放前/冷却中/恢复后）
    log('CD 采样开始 (%ds)：接下来请正常对局，找个机会放一个普通技能（冷却全程自动记录）' % dur)
    series = []
    last_cd_shot = 0.0
    t_end = time.time() + dur
    while time.time() < t_end:
        sh(K + ' skilldump /data/adb/JAY/val/skills.txt', tries=2, timeout=120)
        txt = sh('cat /data/adb/JAY/val/skills.txt', tries=2, timeout=60)
        if not txt.strip():
            time.sleep(0.8)
            continue
        stamp = time.strftime('%Y-%m-%d %H:%M:%S')
        m = re.search(r'start=([0-9:\- ]+) cam_frame=(\S+) cam_valid=(\S+)', txt)
        cam_frame = m.group(2) if m else ''
        hero_id = ''
        for line in txt.splitlines():
            m3 = re.match(r'\s+Hero\[(\d+)\].*?id=\((\d+),(\d+)\)', line)
            if m3:
                hero_id = m3.group(2)
            elif re.match(r'\s+Hero\[(\d+)\].*id=ERR', line):
                hero_id = ''
            m4 = re.match(r'\s+slot\[(\d+)\] type=(\d+) lvl=(\d+) cd=(-?\d+) cdmax=(-?\d+) maxcd=(-?\d+) ready=(\d+)', line)
            if m4 and hero_id:
                series.append(dict(time=stamp, cam_frame=cam_frame, hero=hero_id,
                                   slot=int(m4.group(1)), slot_type=int(m4.group(2)),
                                   lvl=int(m4.group(3)), cur_cd=int(m4.group(4)),
                                   cd_max=int(m4.group(5)), max_cd=int(m4.group(6)),
                                   ready=int(m4.group(7))))
        if any(r['cur_cd'] > 0 for r in series) and (time.time() - last_cd_shot) > 30:
            for cand in ('shot_cd.png', 'shot_cd2.png', 'shot_cd3.png'):
                if not os.path.exists(os.path.join(OUT, cand)):
                    if screenshot(cand):
                        last_cd_shot = time.time()
                    break
        time.sleep(0.6)
    # 相位标注（按 hero+slot 分组）：cd>0 -> during；首次跳变前的 0 -> pre；跳变后的 0 -> post
    groups = defaultdict(list)
    for r in series:
        groups[(r['hero'], r['slot'])].append(r)
    for key, recs in groups.items():
        seen_jump = False
        for r in recs:
            if r['cur_cd'] > 0:
                r['phase'] = 'during'
                seen_jump = True
            elif not seen_jump:
                r['phase'] = 'pre'
            else:
                r['phase'] = 'post'
    with open(os.path.join(OUT, 'cd.jsonl'), 'w', encoding='utf-8') as f:
        for r in series:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
    n_jump = sum(1 for r in series if r['cur_cd'] > 0)
    log('cd.jsonl (%d 条, 冷却中样本 %d 条)' % (len(series), n_jump))
    # 5) 全量快照 + 逐块读取状态
    sh('mkdir -p /data/adb/JAY/val')
    ok = False
    for attempt in range(3):
        out = sh(K + ' dumpgame /data/adb/JAY/val/game.bin /data/adb/JAY/val/game.idx /data/adb/JAY/val/read-status.jsonl',
                 tries=2, timeout=1800)
        tail = [l for l in out.splitlines() if 'dumpgame' in l or '选中' in l]
        log('dumpgame try%d: %s' % (attempt + 1, tail[-2:] if tail else out[-150:]))
        if 'dumpgame ->' in out:
            ok = True
            break
        time.sleep(2)
    if not ok:
        log('!! dumpgame 失败')
    # 6) 拉回快照三件
    if ok:
        for name in ('game.idx', 'read-status.jsonl', 'game.bin'):
            for _ in range(6):
                sh('cp /data/adb/JAY/val/%s /sdcard/_v01_%s' % (name, name))
                ls = sh('ls -la /sdcard/_v01_%s' % name)
                if name in ls and 'No such' not in ls:
                    break
                time.sleep(0.5)
            dst = os.path.join(OUT, name)
            pulled = False
            for _ in range(6):
                p = subprocess.run([ADB, 'pull', '/sdcard/_v01_' + name, dst], capture_output=True, timeout=600)
                if p.returncode == 0 and os.path.exists(dst) and os.path.getsize(dst) > 0:
                    log('拉回 %s (%d B)' % (name, os.path.getsize(dst)))
                    pulled = True
                    break
                time.sleep(0.6)
            if not pulled:
                log('!! 拉回失败: %s' % name)
            sh('rm -f /sdcard/_v01_%s' % name)
    # 7) 运行时 metadata 导出（按当次 maps 的实际映射路径）
    md = ''
    for line in mp.splitlines():
        if 'global-metadata' in line:
            parts = line.split()
            for pth in parts:
                if pth.startswith('/') and 'global-metadata' in pth:
                    md = pth
                    break
            if md:
                break
    if md:
        os.makedirs(os.path.join(OUT, 'metadata'), exist_ok=True)
        # 运行时文件全路径含 $ 符号, 会被 shell 展开 -> 用 * 通配符规避(不加引号)
        sh('cp %s /sdcard/_v01_meta.dat' % md.replace('$', '*'))
        for _ in range(5):
            p = subprocess.run([ADB, 'pull', '/sdcard/_v01_meta.dat', os.path.join(OUT, 'metadata', 'global-metadata.dat')],
                               capture_output=True, timeout=600)
            if p.returncode == 0 and os.path.getsize(os.path.join(OUT, 'metadata', 'global-metadata.dat')) > 0:
                break
            time.sleep(0.6)
        sh('rm -f /sdcard/_v01_meta.dat')
        mpath = os.path.join(OUT, 'metadata', 'global-metadata.dat')
        if os.path.exists(mpath) and os.path.getsize(mpath) > 0:
            with open(os.path.join(OUT, 'metadata-source.txt'), 'w', encoding='utf-8') as f:
                f.write(md + '\n')
            log('metadata 导出: %s (%d B)' % (md, os.path.getsize(mpath)))
        else:
            log('!! metadata 拉回失败: %s' % md)
    else:
        log('!! maps 中未找到 global-metadata 映射')
    log('=== run-01 采集完成 (%.0fs) ===' % (time.time() - t_start))


if __name__ == '__main__':
    main()
