"""Disassemble a TNA game's ARM code. Usage: disasm.py GAME.prc START END [thumb]"""
import sys, gzip
sys.path.insert(0, 'tools'); import prc
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
p = prc.parse(open(sys.argv[1], 'rb').read())
code = None
for t, i, o, d in p['ents']:
    if t == 'ARMZ' and i == 1: code = gzip.decompress(d)
    if t == 'ARMC' and i == 1: code = d
names = [l.strip() for l in open('tools/twglue_names.txt')]
s, e = int(sys.argv[2], 16), int(sys.argv[3], 16)
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if len(sys.argv) > 4 else CS_MODE_ARM)
for x in md.disasm(code[s:e], s):
    note = ''
    if x.mnemonic == 'ldr' and '[pc' in x.op_str:
        try:
            off = int(x.op_str.split('#')[1].rstrip(']'), 0)
            lit = int.from_bytes(code[(x.address + 8 + off) & ~3:(x.address + 8 + off & ~3) + 4], 'little')
            note = f'; ={lit:#x}'
        except Exception: pass
    print(f'{x.address:06x}  {x.bytes.hex():8}  {x.mnemonic:7} {x.op_str}{note}')
