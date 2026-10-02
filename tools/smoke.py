"""Run every TNA game headless for a few seconds; collect crashes, UNIMPL calls and screenshots.
Usage: python tools/smoke.py [seconds] [filter]"""
import glob, os, re, subprocess, sys, gzip
sys.path.insert(0, 'tools'); import prc
from PIL import Image
secs = int(sys.argv[1]) if len(sys.argv) > 1 else 6
flt = sys.argv[2] if len(sys.argv) > 2 else ''
os.makedirs('shots/smoke', exist_ok=True)
env = dict(os.environ, PATH=r'C:\msys64\mingw64\bin;' + os.environ['PATH'])
games = []
for f in sorted(glob.glob('games/**/*.prc', recursive=True)):
    try: p = prc.parse(open(f, 'rb').read())
    except Exception: continue
    for t, i, o, d in p['ents']:
        if t in ('ARMZ', 'ARMC') and i == 1:
            try: c = gzip.decompress(d) if t == 'ARMZ' else d
            except Exception: continue
            if c[28:32] == b'ANWT': games.append(f)
for f in games:
    if flt and flt.lower() not in f.lower(): continue
    name = os.path.splitext(os.path.basename(f))[0]
    shot = os.path.abspath(f'shots/smoke/{name}.bmp')
    slug = os.path.normpath(f).split(os.sep)[1]
    card = ['--card', os.path.join('cards', slug)] if os.path.isdir(os.path.join('cards', slug)) else []
    try:
        r = subprocess.run([os.path.abspath('build/ecliptic.exe'), f, '--headless', '--exit-after', str(secs * 1000), '--screenshot', shot] + card,
                           capture_output=True, text=True, timeout=secs + 20, env=env, errors='replace')
        out = r.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stderr if isinstance(e.stderr, str) else (e.stderr or b'').decode('latin1')) + '\nTIMEOUT'
    open(shot[:-4] + '.log', 'w', encoding='utf-8').write(out)
    errs = [l for l in out.splitlines() if l.startswith('!!') or 'TIMEOUT' in l]
    unimpl = sorted(set(re.findall(r'UNIMPL \[\d+\] \S+ (\w+)\(', out)))
    status = 'HANG' if 'watchdog' in out else 'CRASH' if any('invalid' in e or 'error' in e for e in errs) else 'ok'
    print(f'{status:5} {name:28} unimpl={",".join(unimpl)}')
    for e in errs[:3]: print('        ', e)
    if os.path.exists(shot):
        Image.open(shot).save(shot[:-4] + '.png'); os.remove(shot)
