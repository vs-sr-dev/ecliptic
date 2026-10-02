"""Find literal-pool references to a code offset (TNA code uses sl-relative offsets).
Usage: xref.py GAME.prc OFFSET_HEX | --str TEXT"""
import sys, gzip, struct
sys.path.insert(0, 'tools'); import prc
p = prc.parse(open(sys.argv[1], 'rb').read())
code = None
for t, i, o, d in p['ents']:
    if t == 'ARMZ' and i == 1: code = gzip.decompress(d)
    if t == 'ARMC' and i == 1: code = d
if sys.argv[2] == '--str':
    s = sys.argv[3].encode()
    i = code.find(s)
    while i >= 0:
        print(f'string at {i:#x}')
        targets = [i]
        i = code.find(s, i + 1)
        for tgt in targets:
            for off in range(0, len(code) - 3, 4):
                if struct.unpack_from('<I', code, off)[0] == tgt: print(f'  literal {tgt:#x} at {off:#x}')
else:
    tgt = int(sys.argv[2], 16)
    for off in range(0, len(code) - 3, 4):
        if struct.unpack_from('<I', code, off)[0] == tgt: print(f'literal at {off:#x}')

def ldr_refs(code, lit_off):
    """ARM 'ldr rX, [pc, #imm]' instructions loading the word at lit_off."""
    out = []
    for off in range(max(0, lit_off - 4100), lit_off, 4):
        w = struct.unpack_from('<I', code, off)[0]
        if (w & 0x0F7F0000) == 0x051F0000:   # ldr rX, [pc, #+/-imm]
            imm = w & 0xFFF
            tgt = off + 8 + (imm if w & 0x00800000 else -imm)
            if tgt == lit_off: out.append(off)
    return out
if len(sys.argv) > 4 and sys.argv[4] == '--who':
    pass
