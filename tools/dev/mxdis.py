"""Name the instructions of a Maxwell shader stage dumped by the renderer (SMO_SHADERDUMP vk_fail_*.bin / b*.bin), using the
opcode table of runtime/mx_ops.h. Prints index, raw word, op name and the common register fields (rd 0-7, ra 8-15, rb 20-27,
cbuf bank 34-38 / offset 20-33 for cbuf forms). Not a full disassembler: enough to follow where a register comes from.

    python tools/dev/mxdis.py <file.bin> [--header 0x50] [--max 64]
"""
import argparse
import os
import re
import struct

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_table():
    src = open(os.path.join(REPO, 'runtime', 'gfx', 'mx_ops.h'), encoding='utf-8').read()
    enum = re.search(r'enum\s*\{(.*?)\}', src, re.S).group(1)
    ops = [x.strip().split('=')[0].strip() for x in enum.split(',') if x.strip().startswith('MXOP_')]
    names = re.search(r'mx_op_name\[MXOP_COUNT\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
    names = re.findall(r'"([^"]*)"', names)
    table = [(int(m, 16), int(v, 16), op) for m, v, op in re.findall(r'\{0x([0-9a-f]+)ull,\s*0x([0-9a-f]+)ull,\s*(MXOP_\w+)\}', src)]
    return {op: names[i] for i, op in enumerate(ops) if i < len(names)}, table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--header', type=lambda x: int(x, 0), default=0x50)
    ap.add_argument('--max', type=int, default=64)
    a = ap.parse_args()
    names, table = load_table()
    d = open(a.file, 'rb').read()[a.header:]
    for k in range(min(len(d) // 8, a.max)):
        w, = struct.unpack_from('<Q', d, k * 8)
        if k % 4 == 0:
            print('%4d  %016x  (scheduling)' % (k, w))
            continue
        op = next((o for m, v, o in table if w & m == v), None)
        nm = names.get(op, '?') if op else '?'
        f = 'rd r%d ra r%d rb r%d' % (w & 0xff, w >> 8 & 0xff, w >> 20 & 0xff)
        if 'cbuf' in nm or nm.startswith('LDC'):
            f += ' | c[%d][0x%x]' % (w >> 34 & 0x1f, (w >> 20 & 0x3fff) * 4)
        print('%4d  %016x  %-16s %s' % (k, w, nm, f))


if __name__ == '__main__':
    main()
