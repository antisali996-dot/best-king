# -*- coding: utf-8 -*-
# match_capture_run02.py — 轻量对照采集（不做大快照, 对应老师 run-01 复核 §6 两个口子）
#   A. 静止场景配对: 截图×2 夹 framesnap -> framesnap1.json + projection_points.json(全单位投影点)
#      记录: 时间/frame/VP/本地英雄(相机中心)/最近塔及其投影落点
#   B. 同期短 CD 片段: cd.jsonl(连续, 字段同 run-01) + 本地英雄跳变即拍 -> cd_shots.jsonl + shot_cd_NN.png
#      逐图保留: 时间(含 t_epoch)/frame/对象键/槽号/cur_cd/cd_max/max_cd/ready/面板对象
# 用法: python match_capture_run02.py [cd_seconds=240]
import subprocess, time, sys, os, re, json

ADB = r'D:\OPENCODE\workspace\xiaogu-kernel\tools\_vendor\platform-tools\adb.exe'
K = '/system/bin/linker64 /data/adb/JAY/King.sh'
PKG = 'com.tencent.tmgp.sgame'
OUT = r'D:\OPENCODE\workspace\android-memory-research\external\validation\run-02'
WARMUP = 20
W, H = 1712.0, 770.0


def sh(cmd, tries=6, timeout=900, raw=False):
    for _ in range(tries):
        try:
            args = [ADB] + ([('exec-out' if raw else 'shell'), 'su', '-c', 'nsenter -t 1 -m -- ' + cmd])
            p = subprocess.run(args, capture_output=True, timeout=timeout)
            if raw:
                if p.stdout:
                    return p.stdout
                continue
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
        log('截图 %s (%d B) t_epoch=%.3f' % (name, len(b), time.time()))
        return True
    log('!! 截图失败 %s (%d B)' % (name, len(b)))
    return False


def proj(vp, p):
    x, y, z = p
    cx = vp[0] * x + vp[4] * y + vp[8] * z + vp[12]
    cy = vp[1] * x + vp[5] * y + vp[9] * z + vp[13]
    cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15]
    if cw <= 1e-6:
        return None
    nx, ny = cx / cw, cy / cw
    return nx, ny, (nx + 1) * W / 2, (1 - ny) * H / 2


def parse_skilldump(txt):
    m = re.search(r'cam_frame=(\S+) cam_valid=(\S+)', txt)
    cf = m.group(1) if m else '?'
    rows = []
    hero = ''
    for line in txt.splitlines():
        m3 = re.match(r'\s+Hero\[(\d+)\].*?id=\((\d+),(\d+)\)', line)
        if m3:
            hero = m3.group(2)          # 与 run-01 cd.jsonl 一致: 只记 coreSeq
            continue
        if re.match(r'\s+Hero\[(\d+)\].*id=ERR', line):
            hero = ''
            continue
        m4 = re.match(r'\s+slot\[(\d+)\] type=(\d+) lvl=(\d+) cd=(-?\d+) cdmax=(-?\d+) maxcd=(-?\d+) ready=(\d+)', line)
        if m4 and hero:
            rows.append(dict(hero=hero, slot=int(m4.group(1)), slot_type=int(m4.group(2)),
                             lvl=int(m4.group(3)), cur_cd=int(m4.group(4)), cd_max=int(m4.group(5)),
                             max_cd=int(m4.group(6)), ready=int(m4.group(7))))
    return cf, rows


def main():
    dur = int(sys.argv[1]) if len(sys.argv) > 1 else 240
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, 'run-info.txt'), 'w', encoding='utf-8').close()
    t0 = time.time()
    log('=== run-02 轻量对照采集开始（不做大快照）===')
    info = sh('dumpsys package %s | grep -m2 versionName; pidof %s; wm size; wm density' % (PKG, PKG))
    log('设备信息:\n' + info.strip())
    mp = sh(K + ' maps')
    open(os.path.join(OUT, 'maps.txt'), 'w', encoding='utf-8').write(mp)
    log('maps.txt (%d 行)' % len(mp.splitlines()))

    # ---------- A. 静止场景配对（英雄站定, 靠近可见的塔） ----------
    log('A 阶段: 请站定不动 %d 秒（英雄尽量靠近一座可见的塔）' % WARMUP)
    time.sleep(WARMUP)
    screenshot('shot_a1.png')
    sh(K + ' framesnap /data/adb/JAY/val/fs_r2.json')
    fs = sh('cat /data/adb/JAY/val/fs_r2.json')
    local = None
    if fs.strip():
        open(os.path.join(OUT, 'framesnap1.json'), 'w', encoding='utf-8').write(fs)
        d = json.loads(fs)
        vp = d['vp']
        log('framesnap1: time=%s frame=%s valid=%s' % (d['time'], d['cam_frame'], d['cam_valid']))
        best = None
        pts = []
        for lst in ('Hero', 'Tower', 'Soldier', 'BuffMonster'):
            for r in d['lists'][lst]:
                if not r['pos']:
                    pts.append(dict(list=lst, id=r['id'], pos=None))
                    continue
                pr = proj(vp, r['pos'])
                if pr is None:
                    pts.append(dict(list=lst, id=r['id'], pos=r['pos'], ndc=None, pxpy=None))
                    continue
                nx, ny, px, py = pr
                pts.append(dict(list=lst, id=r['id'], pos=r['pos'],
                                ndc=[round(nx, 4), round(ny, 4)], pxpy=[round(px, 1), round(py, 1)]))
                if lst == 'Hero' and (best is None or abs(nx) + abs(ny) < best[0]):
                    best = (abs(nx) + abs(ny), r['id'], r['pos'], (nx, ny, px, py))
        with open(os.path.join(OUT, 'projection_points.json'), 'w', encoding='utf-8') as f:
            json.dump(dict(time=d['time'], cam_frame=d['cam_frame'], cam_valid=d['cam_valid'],
                           viewport=[W, H], vp=vp, points=pts), f, ensure_ascii=False, indent=1)
        if best:
            local = str(best[1][0])
            log('本地英雄(相机跟随/屏幕中心最近): coreSeq=%s 对象键=(%s,%s) pos=%s ndc=(%.3f,%.3f) px=(%.0f,%.0f)'
                % (local, best[1][0], best[1][1], best[2], best[3][0], best[3][1], best[3][2], best[3][3]))
            # 最近的塔（独立参照, 验证跟随对象之外的目标落位）
            lh_pos = best[2]
            bestt = None
            for t in d['lists']['Tower']:
                if not t['pos']:
                    continue
                dd = ((t['pos'][0] - lh_pos[0]) ** 2 + (t['pos'][2] - lh_pos[2]) ** 2) ** 0.5
                if bestt is None or dd < bestt[0]:
                    bestt = (dd, t)
            if bestt:
                pr = proj(vp, bestt[1]['pos'])
                if pr:
                    log('最近塔 id=%s dist=%.1f pos=%s 投影 ndc=(%.3f,%.3f) px=(%.0f,%.0f) %s'
                        % (bestt[1]['id'], bestt[0], bestt[1]['pos'], pr[0], pr[1], pr[2], pr[3],
                           'IN-VIEW' if -1 <= pr[0] <= 1 and -1 <= pr[1] <= 1 else 'OUT-OF-VIEW'))
                else:
                    log('最近塔 id=%s dist=%.1f 在相机后方(cw<=0)（参照请用画面内其他固定物）' % (bestt[1]['id'], bestt[0]))
    else:
        log('!! framesnap 输出为空')
    screenshot('shot_a2.png')

    # ---------- B. 同期短 CD 片段 ----------
    log('B 阶段: 连续 CD %d 秒; 本地英雄(=%s)放技能即自动抓图（cd_shots.jsonl 记同刻数值）' % (dur, local))
    series = []
    shots = []
    prev = {}
    local_shots = 0
    t_b_start = time.time()
    last_shot = 0.0
    t_end = time.time() + dur
    while time.time() < t_end:
        sh(K + ' skilldump /data/adb/JAY/val/sk_r2.txt', tries=2, timeout=120)
        txt = sh('cat /data/adb/JAY/val/sk_r2.txt', tries=2, timeout=60)
        if not txt.strip():
            time.sleep(0.8)
            continue
        stamp = time.strftime('%Y-%m-%d %H:%M:%S')
        cf, rows = parse_skilldump(txt)
        for r in rows:
            series.append(dict(time=stamp, cam_frame=cf, **r))
        jump_local = None
        jump_other = None
        for r in rows:
            key = (r['hero'], r['slot'])
            if r['cur_cd'] > 0 and prev.get(key, 0) <= 0:
                if local and r['hero'] == local:
                    jump_local = r
                elif r['cur_cd'] >= 3000:
                    jump_other = r
            prev[key] = r['cur_cd']
        take = None
        panel = ''
        if jump_local and local_shots < 4:
            take, panel = jump_local, '本地英雄(相机中心跟随对象)'
        elif jump_other and local_shots == 0 and (time.time() - t_b_start) > 60 and len(shots) < 2:
            take, panel = jump_other, 'UNKNOWN-候选hero=%s(本地未命中,请按截图排除)' % jump_other['hero']
        if take and len(shots) < 6 and (time.time() - last_shot) > 3:
            name = 'shot_cd_%02d.png' % (len(shots) + 1)
            if screenshot(name):
                last_shot = time.time()
                if take is jump_local:
                    local_shots += 1
                rec = dict(time=stamp, t_epoch=round(time.time(), 3), cam_frame=cf,
                           hero=take['hero'], slot=take['slot'], slot_type=take['slot_type'],
                           lvl=take['lvl'], cur_cd=take['cur_cd'], cd_max=take['cd_max'],
                           max_cd=take['max_cd'], ready=take['ready'], shot=name, panel=panel)
                shots.append(rec)
                log('CD跳变抓图 %s slot=%d cur_cd=%d cd_max=%d max_cd=%d frame=%s panel=%s'
                    % (name, take['slot'], take['cur_cd'], take['cd_max'], take['max_cd'], cf, panel))
        time.sleep(0.6)
    with open(os.path.join(OUT, 'cd.jsonl'), 'w', encoding='utf-8') as f:
        for r in series:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
    with open(os.path.join(OUT, 'cd_shots.jsonl'), 'w', encoding='utf-8') as f:
        for r in shots:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
    log('cd.jsonl %d 条; cd_shots.jsonl %d 张(本地 %d); 本地英雄=%s' % (len(series), len(shots), local_shots, local))
    log('=== run-02 完成 (%.0fs) ===' % (time.time() - t0))


if __name__ == '__main__':
    main()
