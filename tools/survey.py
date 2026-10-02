"""Survey all PRCs inside the tapwave.net archive zips: classify TNA/ARMlet/68k."""
import zipfile, glob, gzip, sys, io, collections
sys.path.insert(0, 'tools'); import prc
rows = []
def scan(name, data, src):
    try: p = prc.parse(data)
    except Exception as e: return
    if not p['res']: return
    types = collections.Counter(e[0] for e in p['ents'])
    kind = []
    for t, i, o, d in p['ents']:
        if t in ('ARMZ', 'ARMC'):
            code = gzip.decompress(d) if t == 'ARMZ' else d
            magic = code[28:32]
            kind.append(f"{t}{i}:{len(code)}:{magic.decode('latin1') if magic.isascii() else magic.hex()}")
    if 'ARML' in types: kind.append(f"ARML x{types['ARML']}")
    if 'armc' in types: kind.append(f"armc x{types['armc']}")
    if 'code' in types: kind.append(f"68k code x{types['code']} ({sum(len(e[3]) for e in p['ents'] if e[0]=='code')}B)")
    rows.append((src, name, p['type'], p['creator'], ' '.join(kind)))
for z in sorted(glob.glob('archive/files/*/dl/*.zip')):
    try: zf = zipfile.ZipFile(z)
    except Exception: continue
    for i in zf.infolist():
        if '__MACOSX' in i.filename or not i.filename.lower().endswith('.prc'): continue
        scan(i.filename.split('/')[-1], zf.read(i), z.replace(chr(92),'/').split('/')[2])
for f in glob.glob('archive/files/*/dl/*.prc'):
    scan(f.replace(chr(92),'/').split('/')[-1], open(f,'rb').read(), f.replace(chr(92),'/').split('/')[2])
for r in rows: print(f"{r[0][:22]:22} {r[1][:30]:30} {r[2]} {r[3]} {r[4]}")
