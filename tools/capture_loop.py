"""capture_loop.py: unattended captures in the grass, to reproduce what breaks the world after a capture (HISTORICO 60).

  capture_loop.py run <name> [--game eevee|pikachu] [--frames N] [--save <savedata.bin>] [--fps 60|30] [--turbo 0|1] [--leg <frames>]
                  [VAR=value ...]          one session: walks back and forth in the grass, looped; inside an encounter
                                           input.c presses B to run away; (LARECOMP_INPUT_LOOP); after each battle /
                                           capture it holds still for 300 frames (input.c) so the view can be compared
                                           with the one before the encounter; EXE=<path> runs another build,
                                           DATA=<folder> another data root (an install's extracted/)
  capture_loop.py scan <name>              per encounter: the frame before it against the frame after the world is back
                                           (luma, 4 px tolerance: % of pixels off by > 24) and build/capture_loop/<name>/
                                           pairs_NN.png (before | after, 4 encounters per image) to look at

Frames go to one raw file (LARECOMP_SHOT_RAW, every 10th frame, 480x270). Settings: fps = 60 by default (as the player who
found the bug); VAR=value pairs go to the environment (LARECOMP_*) or to larecomp.ini (anything else: render_thread=0,
vertex_read=direct). The save is copied (default: the Eevee install's, D:/Jogos/LetsGoRecomp-Eevee/save).
"""
import os
import re
import struct
import subprocess
import sys
import time
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bateria_scan import load, luma, novelty, W, H  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'capture_loop')
BASE = '400:A:6,500:A:6,1150:A:6,1300:A:6'  # controller screen, title, continue
LOOP_START = 1500


def script(leg=80):
    e = [BASE]
    t = LOOP_START
    for k in range(6):  # back and forth in six directions (returns to the start; --leg 300 walks across areas)
        a = (k * 60 + 30) % 360
        e.append(f'{t}:STICK_{a}:{leg}'); t += leg
        e.append(f'{t}:STICK_{(a + 180) % 360}:{leg}'); t += leg
    # (no A: it talks to the partner Pokemon; inside an encounter input.c presses B to run away, no capture needed)
    return ','.join(e)


def run(name, game, frames, save, fps, turbo, extra, leg=80):
    out = os.path.join(OUT, name)
    os.makedirs(os.path.join(out, 'save', 'save_0'), exist_ok=True)
    with open(save, 'rb') as f, open(os.path.join(out, 'save', 'save_0', 'savedata.bin'), 'wb') as g:
        g.write(f.read())
    exe = extra.pop('EXE', os.path.join(ROOT, 'build', 'lasrecomp.exe'))  # (EXE=: another build or a copy under its own name)
    ini_keys = {k: v for k, v in extra.items() if not k.startswith('LARECOMP_')}
    env_keys = {k: v for k, v in extra.items() if k.startswith('LARECOMP_')}
    ini = os.path.join(out, 'larecomp.ini')
    with open(ini, 'w') as f:
        f.write(f'fps = {fps}\nwild_battle = 0\nrender_height = 0\nresolution = match\nsmaa = 1\nshow_fps = 0\n')
        for k, v in ini_keys.items():
            f.write(f'{k} = {v}\n')
    raw, log = os.path.join(out, 'frames.raw'), os.path.join(out, 'log.txt')
    env = dict(os.environ, LARECOMP_NO_AUDIO='1', LARECOMP_NO_USER_INPUT='1', LARECOMP_NO_LAUNCHER='1',
               LARECOMP_SAVE_DIR=os.path.join(out, 'save'), LARECOMP_SETTINGS=ini, LARECOMP_SCALE='0.5',
               LARECOMP_INPUT=script(leg), LARECOMP_INPUT_LOOP=f'{LOOP_START}:{12 * leg}',
               LARECOMP_SCREENSHOT_EVERY='10', LARECOMP_SCREENSHOT_FROM='1400', LARECOMP_SHOT_RAW=raw,
               LARECOMP_FRAMETIME_CSV=os.path.join(out, 'frametimes.csv'), LARECOMP_EXIT_AT_PRESENT=str(frames))
    env['PATH'] = 'C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + env.get('PATH', '')
    if turbo:
        env['LARECOMP_FPS'] = '0'
    root = extra.pop('DATA', 'extracted')  # (DATA=<folder>: another install's data root, e.g. a release set up with an update)
    if game == 'pikachu':
        root = 'extracted-pikachu'
        env['LARECOMP_GAME_LIB'] = os.path.join(ROOT, 'extracted-pikachu', 'lasgame.dll')
    env.update(env_keys)
    t0 = time.time()
    with open(log, 'w') as lf:
        p = subprocess.Popen([exe, root, '--play'], cwd=ROOT, env=env,
                             stdout=lf, stderr=subprocess.STDOUT)
        txt = ''
        while time.time() - t0 < 3600:
            time.sleep(2)
            with open(log, errors='replace') as r:
                txt = r.read()
            if 'exit at present' in txt or 'FATAL' in txt or 'exception c0' in txt:
                break
        time.sleep(1)
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)], capture_output=True)
    n = len(re.findall(r'\[scene\] start', txt))
    print(f'{name}: {time.time() - t0:.0f}s, {n} encounters, {"crash" if "FATAL" in txt or "exception c0" in txt else "ok"}')


def png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\x00' + rgb[y].tobytes() for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def scan(name):
    out = os.path.join(OUT, name)
    txt = open(os.path.join(out, 'log.txt'), errors='replace').read()
    starts = [int(x) for x in re.findall(r'\[scene\] start (\d+)', txt)]
    backs = [int(x) for x in re.findall(r'\[scene\] world again (\d+)', txt)]
    index, pix = load(os.path.join(out, 'frames.raw'))
    frames = sorted(index)
    near = lambda f: max([x for x in frames if x <= f], default=None)
    pairs = []
    for s in starts:
        b = min([x for x in backs if x > s], default=None)
        if b is None:
            continue
        fb, fa = near(s - 4), near(b + 250)
        if fb is None or fa is None or fa < b:
            continue
        x, y = luma(pix, index[fb]), luma(pix, index[fa])
        d = float((np.minimum(novelty(y, x), novelty(x, y)) > 24).mean() * 100)
        pairs.append((s, b, fb, fa, d))
    for i, (s, b, fb, fa, d) in enumerate(pairs):
        print(f'encounter {i:2d}: start {s} back {b}  before {fb} after {fa}  differ {d:5.2f}%')
    for k in range(0, len(pairs), 4):
        rows = [np.concatenate([pix[index[p[2]]], np.full((H, 4, 3), 255, np.uint8), pix[index[p[3]]]], axis=1)
                for p in pairs[k:k + 4]]
        png(os.path.join(out, f'pairs_{k // 4:02d}.png'), np.concatenate(rows, axis=0))
    return pairs


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a or a[0] not in ('run', 'scan'):
        sys.exit(__doc__)
    if a[0] == 'scan':
        scan(a[1])
        sys.exit()
    name, rest = a[1], a[2:]
    opt = {'--game': 'eevee', '--frames': '20000', '--save': 'D:/Jogos/LetsGoRecomp-Eevee/save/save_0/savedata.bin',
           '--fps': '60', '--turbo': '1', '--leg': '80'}
    extra = {}
    i = 0
    while i < len(rest):
        if rest[i] in opt:
            opt[rest[i]] = rest[i + 1]; i += 2
        else:
            k, _, v = rest[i].partition('='); extra[k] = v; i += 1
    run(name, opt['--game'], int(opt['--frames']), opt['--save'], int(opt['--fps']), opt['--turbo'] == '1', extra,
        int(opt['--leg']))
