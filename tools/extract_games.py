"""Extract every game/homebrew zip of the tapwave.net archive into games/<slug>/."""
import zipfile, glob, os
SKIP = ('tapwave-zodiac-sdk', 'tapwave-zodiac-hdk', 'zodiac-1-1-rom-update', 'zodiac-desktop-software')
def slug_of(p): return os.path.normpath(p).split(os.sep)[2]
for z in sorted(glob.glob('archive/files/*/dl/*.zip')):
    slug = slug_of(z)
    if slug in SKIP: continue
    try: zf = zipfile.ZipFile(z)
    except zipfile.BadZipFile: print("bad zip:", z); continue
    for i in zf.infolist():
        if i.is_dir() or '__MACOSX' in i.filename or i.filename.endswith('.DS_Store'): continue
        out = os.path.join('games', slug, i.filename.replace(':', '_'))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, 'wb').write(zf.read(i))
for f in glob.glob('archive/files/*/dl/*.prc'):
    os.makedirs(os.path.join('games', slug_of(f)), exist_ok=True)
    open(os.path.join('games', slug_of(f), os.path.basename(f)), 'wb').write(open(f, 'rb').read())
