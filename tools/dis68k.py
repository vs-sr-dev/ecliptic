"""Disassemble a 68k 'code' resource, decoding Palm 'trap #15; dc.w sysTrapXXX' pairs.
Usage: dis68k.py FILE.prc [codeID]"""
import sys, re
sys.path.insert(0, 'tools'); import prc
from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_000
traps = {}
for line in open('ref/sdk11/Tapwave 1.1 SDK/Incs/CoreTraps.h', encoding='latin1'):
    m = re.match(r'#define\s+(sysTrap\w+)\s+0x([0-9A-Fa-f]+)', line)
    if m: traps[int(m.group(2), 16)] = m.group(1)
cid = int(sys.argv[2]) if len(sys.argv) > 2 else 1
p = prc.parse(open(sys.argv[1], 'rb').read())
code = [e for e in p['ents'] if e[0] == 'code' and e[1] == cid][0][3]
md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000)
off = 0
while off < len(code):
    ins = list(md.disasm(code[off:off + 16], off, 1))
    if not ins:
        print(f'{off:04x}  {code[off:off+2].hex():12} dc.w'); off += 2; continue
    x = ins[0]
    print(f'{x.address:04x}  {x.bytes.hex():12} {x.mnemonic:8} {x.op_str}')
    off += x.size
    if x.mnemonic == 'trap' and x.op_str == '#$f':
        t = int.from_bytes(code[off:off + 2], 'big')
        print(f'{off:04x}  {code[off:off+2].hex():12} dc.w     {t:#x} {traps.get(t, "")}')
        off += 2
