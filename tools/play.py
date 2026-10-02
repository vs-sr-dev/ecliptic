"""Launch a game from games/<slug> with its virtual SD card.
Usage: python tools/play.py SLUG [extra ecliptic options...]
       python tools/play.py --list"""
import glob, gzip, os, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__))
import prc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tna_prcs(slug):
    out = []
    for f in sorted(glob.glob(os.path.join(ROOT, 'games', slug, '**', '*.prc'), recursive=True)):
        try:
            p = prc.parse(open(f, 'rb').read())
        except Exception:
            continue
        for t, i, o, d in p['ents']:
            if t in ('ARMZ', 'ARMC') and i == 1:
                try:
                    c = gzip.decompress(d) if t == 'ARMZ' else d
                except Exception:
                    continue
                if c[28:32] == b'ANWT':
                    out.append(f)
    return out


def main():
    if len(sys.argv) < 2 or sys.argv[1] == '--list':
        for slug in sorted(os.listdir(os.path.join(ROOT, 'games'))):
            for f in tna_prcs(slug):
                print(f'{slug:24} {os.path.relpath(f, ROOT)}')
        return
    slug = sys.argv[1]
    games = tna_prcs(slug)
    if not games:
        sys.exit(f'no Tapwave Native Application found in games/{slug}')
    cmd = [os.path.join(ROOT, 'build', 'ecliptic.exe'), games[0]]
    card = os.path.join(ROOT, 'cards', slug)
    if os.path.isdir(card):
        cmd += ['--card', card]
    cmd += sys.argv[2:]
    env = dict(os.environ, PATH=r'C:\msys64\mingw64\bin;' + os.environ['PATH'])
    print(' '.join(cmd))
    subprocess.run(cmd, cwd=ROOT, env=env)


if __name__ == '__main__':
    main()
