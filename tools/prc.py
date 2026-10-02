"""Palm PDB/PRC container parser. Usage: prc.py FILE [--dump DIR]"""
import struct, sys, os

def parse(data):
    name = data[:32].split(b'\0')[0].decode('latin1')
    attr, ver = struct.unpack('>HH', data[32:36])
    typ, crea = data[60:64], data[64:68]
    n = struct.unpack('>H', data[76:78])[0]
    is_res = bool(attr & 1)
    ents = []
    off = 78
    for i in range(n):
        if is_res:
            rt, rid, ro = struct.unpack('>4sHI', data[off:off+10]); off += 10
            ents.append([rt.decode('latin1'), rid, ro])
        else:
            ro, ra, uid = struct.unpack('>IB3s', data[off:off+8]); off += 8
            ents.append(['rec', i, ro])
    for i, e in enumerate(ents):
        end = ents[i+1][2] if i+1 < len(ents) else len(data)
        e.append(data[e[2]:end])
    return dict(name=name, attr=attr, ver=ver, type=typ.decode('latin1'), creator=crea.decode('latin1'), res=is_res, ents=ents)

if __name__ == '__main__':
    p = parse(open(sys.argv[1], 'rb').read())
    print(f"name={p['name']!r} type={p['type']} creator={p['creator']} attr={p['attr']:#x} ver={p['ver']} {'PRC' if p['res'] else 'PDB'} entries={len(p['ents'])}")
    for t, i, o, d in p['ents']:
        print(f"  {t} {i:5d} off={o:#09x} len={len(d):8d} head={d[:16].hex()}")
    if '--dump' in sys.argv:
        out = sys.argv[sys.argv.index('--dump')+1]; os.makedirs(out, exist_ok=True)
        for t, i, o, d in p['ents']:
            open(os.path.join(out, f"{t.replace('/','_')}_{i:04x}.bin"), 'wb').write(d)
