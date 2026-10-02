"""Heuristic: find indirect calls through the TwGlue table in a TNA ARM image.
Pattern (CodeWarrior): ldr Rt, [Rb, #imm] ... (mov lr, pc; bx Rt | blx Rt) within a few insns."""
import sys, gzip, collections
sys.path.insert(0, 'tools'); import prc
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
names = [l.strip() for l in open('tools/twglue_names.txt')]
p = prc.parse(open(sys.argv[1], 'rb').read())
code = None
for t, i, o, d in p['ents']:
    if t == 'ARMZ' and i == 1: code = gzip.decompress(d)
    if t == 'ARMC' and i == 1: code = d
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
ins = []
for off in range(0, len(code) - 3, 4):
    w = code[off:off+4]
    try:
        x = next(md.disasm(w, off))
        ins.append((x.mnemonic, x.op_str))
    except StopIteration:
        ins.append(('?', ''))
hist = collections.Counter()
for k, (m, ops) in enumerate(ins):
    if m in ('bx', 'blx') and k > 0:
        reg = ops.strip()
        for j in range(k - 1, max(0, k - 5), -1):
            m2, o2 = ins[j]
            if m2 == 'ldr' and o2.startswith(reg + ',') and '#' in o2 and '[' in o2:
                try: imm = int(o2.split('#')[1].rstrip(']!'), 0)
                except ValueError: break
                if imm % 4 == 0 and imm // 4 < len(names): hist[imm // 4] += 1
                break
print(f"{len(hist)} distinct table slots, {sum(hist.values())} call sites")
for idx, c in sorted(hist.items()):
    print(f"  {idx:4d} {names[idx]:40s} x{c}")
