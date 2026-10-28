"""Make a unique byte signature for a function in the unpacked exe, wildcarding relative operands.
make_sig.py <exe> <va hex> [max bytes]  ->  "48 89 5C 24 ?? ..." and how many .text matches it has."""
import sys, re, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
exe, va = sys.argv[1], int(sys.argv[2], 16)
maxb = int(sys.argv[3]) if len(sys.argv) > 3 else 96
pe = pefile.PE(exe, fast_load=True); ib = pe.OPTIONAL_HEADER.ImageBase
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
code = text.get_data(); t0 = ib + text.VirtualAddress
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
data = pe.get_data(va - ib, maxb + 16)
sig = []
for ins in md.disasm(data, va):
    b = list(ins.bytes)
    mask = [True] * len(b)
    # wildcard rel32 (call/jmp/jcc rel32, rip-relative disp32)
    if ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
        if len(b) >= 5 and ins.op_str.startswith('0x'):
            for k in range(len(b) - 4, len(b)): mask[k] = False
    if 'rip' in ins.op_str:
        off = ins.disp_offset if hasattr(ins, 'disp_offset') and ins.disp_offset else None
        if off:
            for k in range(off, off + 4): mask[k] = False
    sig += [(x if m else None) for x, m in zip(b, mask)]
    if len(sig) >= maxb: break
def count(s):
    pat = b''.join(re.escape(bytes([x])) if x is not None else b'.' for x in s)
    return len(re.findall(pat, code, re.S))
# shortest unique prefix
for n in range(16, len(sig) + 1, 4):
    if count(sig[:n]) == 1:
        sig = sig[:n]; break
print(' '.join(f'{x:02X}' if x is not None else '??' for x in sig))
print('matches:', count(sig))
