# -*- coding: utf-8 -*-
"""
snap_diff.py — King_snap.sh 日志解析 + 差分分析器

用法: python snap_diff.py <live_*.log>
输出:
  1) 链条头信息
  2) 十英雄坐标时序 + 移动检测（哪几帧之间动了、位移量）
  3) HEX 块逐帧差分（P/X1/X2/T/A/C1..C4）:
     - 每个变化字 (块内偏移, 旧值, 新值, 变化量)
     - 标记: 下降(DOWN)/上升(UP)、hp 量级候选(100..30000)
  4) 自动结论建议（坐标活性 / 血量候选偏移）
"""
import sys, re, json
from collections import OrderedDict

try:
    sys.stdout.reconfigure(encoding='utf-8')
except Exception:
    pass

def parse_log(path):
    txt = open(path, encoding='utf-8', errors='replace').read()
    out = {'header': [], 'table': [], 'frames': []}
    for ln in txt.splitlines():
        if re.match(r'^\[[0-9]\] ', ln) or ln.startswith('[!]'):
            out['header'].append(ln.strip())
        m = re.match(r'^\s*\[(\d+)\] (\d+) (\d+) (\d+) (\d+) (0x[0-9A-F]+) (0x[0-9A-F]+) (-?\d+|ERR) (-?\d+|ERR)(.*)$', ln)
        if m:
            out['table'].append(dict(idx=int(m.group(1)), uid=int(m.group(2)), hid=int(m.group(3)),
                                     camp=int(m.group(4)), slot=int(m.group(5)),
                                     P=m.group(6), X2=m.group(7),
                                     x=m.group(8), z=m.group(9), warn=m.group(10).strip()))
    # frames
    parts = re.split(r'^===== FRAME (\d+)/(\d+)\s+([\d:]*)\s*=====$', txt, flags=re.M)
    # parts: [pre, fidx, ftot, time, body, fidx, ftot, time, body, ...]
    for i in range(1, len(parts), 4):
        fidx, ftot, ftime, body = int(parts[i]), int(parts[i+1]), parts[i+2], parts[i+3]
        frame = dict(idx=fidx, total=ftot, time=ftime, pos={}, hex=OrderedDict())
        for m in re.finditer(r'^POS (\d+) (\d+) (-?\d+) (-?\d+) (\S+) (\S+)$', body, re.M):
            frame['pos'][int(m.group(1))] = dict(uid=int(m.group(2)), hid=int(m.group(3)),
                                                 camp=int(m.group(4)), x=m.group(5), z=m.group(6))
        # hex blocks
        for m in re.finditer(r'^HEX (\S+) ([^\n]*)\n((?:(?!^HEX |^=====).*\n)*)', body, re.M):
            name, hdr, words_txt = m.group(1), m.group(2).strip(), m.group(3)
            words = []
            for w in words_txt.split():
                if re.match(r'^[0-9a-fA-F]{1,8}$', w):
                    words.append(int(w, 16))
            info = {'header': hdr, 'words': words}
            if hdr.endswith('FAIL'):
                info['status'] = 'FAIL'
            elif hdr.strip() == 'SKIP' or hdr == '':
                info['status'] = 'SKIP'
            else:
                info['status'] = 'OK'
            frame['hex'][name] = info
        out['frames'].append(frame)
    return out

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    log = parse_log(sys.argv[1])
    print('=' * 62)
    print('[头部]')
    for h in log['header']:
        print('  ', h)
    print('=' * 62)
    print('[英雄表]')
    for r in log['table']:
        print(f"  [{r['idx']}] uid={r['uid']} id={r['hid']} camp={r['camp']} slot={r['slot']} "
              f"P={r['P']} X1={r['X2']} init=({r['x']},{r['z']}) {r['warn']}")
    print('=' * 62)
    print(f"[帧数] {len(log['frames'])}")

    # ---- 1) 移动检测 ----
    print('=' * 62)
    print('[坐标时序/移动检测]')
    idxs = sorted({i for f in log['frames'] for i in f['pos']})
    for i in idxs:
        seq = []
        for f in log['frames']:
            p = f['pos'].get(i)
            seq.append((f['idx'], p['x'], p['z']) if p else (f['idx'], None, None))
        valid = [(fi, x, z) for (fi, x, z) in seq if x not in (None, 'ERR')]
        moves = []
        for a, b in zip(valid, valid[1:]):
            try:
                dx = int(b[1]) - int(a[1]); dz = int(b[2]) - int(a[2])
            except ValueError:
                continue
            if dx or dz:
                moves.append(f"f{a[0]}->f{b[0]}: d=({dx:+d},{dz:+d})")
        hero = log['table'][i] if i < len(log['table']) else {}
        tag = 'MOVED' if moves else ('STATIC' if len(valid) >= 2 else 'NO-DATA')
        print(f"  [{i}] id={hero.get('hid')} uid={hero.get('uid')} {tag}"
              + ('  ' + ' | '.join(moves) if moves else ''))

    # ---- 2) HEX 差分 ----
    print('=' * 62)
    print('[HEX 块差分]')
    blk_names = []
    for f in log['frames']:
        for n in f['hex']:
            if n not in blk_names:
                blk_names.append(n)
    cands = []
    for name in blk_names:
        diffs = []
        frames_ok = [(f['idx'], f['hex'][name]) for f in log['frames'] if name in f['hex']]
        for (fa, da), (fb, db) in zip(frames_ok, frames_ok[1:]):
            wa, wb = da['words'], db['words']
            if da['status'] != 'OK' or db['status'] != 'OK':
                continue
            n = min(len(wa), len(wb))
            for k in range(n):
                if wa[k] != wb[k]:
                    off = k * 4
                    old, new = wa[k], wb[k]
                    delta = new - old
                    flags = []
                    if delta < 0: flags.append('DOWN')
                    else: flags.append('UP')
                    if 100 <= old <= 30000 or 100 <= new <= 30000:
                        flags.append('HP尺度')
                    diffs.append((fa, fb, off, old, new, delta, ','.join(flags)))
        print(f'  {name:3s}: ' + (f'{len(diffs)} 处变化' if diffs else '无变化'))
        for (fa, fb, off, old, new, delta, flags) in diffs[:40]:
            print(f'      f{fa}->f{fb} 块内+{off:#06x}: {old:#x}({old}) -> {new:#x}({new}) '
                  f'delta={delta:+d} [{flags}]')
            cands.append(dict(block=name, f_from=fa, f_to=fb, off=off, old=old, new=new, delta=delta))
        if len(diffs) > 40:
            print(f'      ... 共 {len(diffs)} 处（截断显示 40）')

    # ---- 3) 结论 ----
    print('=' * 62)
    print('[自动结论]')
    moved = []
    for i in idxs:
        seq = [(f['idx'], f['pos'].get(i)) for f in log['frames'] if f['pos'].get(i)]
        vv = [(fi, p['x'], p['z']) for (fi, p) in seq if p['x'] not in ('ERR',) and p['z'] not in ('ERR',)]
        for a, b in zip(vv, vv[1:]):
            if a[1] != b[1] or a[2] != b[2]:
                moved.append(i)
                break
    if moved:
        print(f'  · 坐标活性: [OK] 检测到英雄 idx={moved} 发生了坐标变化 -> 位置链是实时的')
    else:
        print('  · 坐标活性: [WARN] 所有帧坐标完全一致（可能全程静止/或链路异常）')
    if cands:
        print(f'  · HEX 差分: 共 {len(cands)} 处变化')
        hp_like = [c for c in cands if c['delta'] < 0 and 100 <= c['old'] <= 30000]
        if hp_like:
            print('  · 血量候选(下降+量级相符):')
            for c in hp_like[:20]:
                print(f"      {c['block']} +{c['off']:#x}: {c['old']} -> {c['new']} (f{c['f_from']}->f{c['f_to']})")
    else:
        print('  · HEX 差分: 无变化（若期间未受伤/未走位属正常）')
    print('=' * 62)
    json.dump({'table': log['table'], 'cands': cands}, open(sys.argv[1] + '.diff.json', 'w', encoding='utf-8'), indent=1)
    print(f"[输出] 机器可读差分: {sys.argv[1]}.diff.json")

if __name__ == '__main__':
    main()
