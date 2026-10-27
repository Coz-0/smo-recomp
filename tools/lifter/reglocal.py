"""Guest general registers of one lifted function in C locals instead of the context (`c->x[n]`).

Why: every guest store goes through memcpy to a pointer the compiler cannot tell apart from the Ctx, so with the
registers in `c->x` each store forces every later register read back from memory, and the loops' gotos make LLVM's
loop passes walk thousands of such loads. In locals, SROA keeps them in host registers.

The context stays the truth at every point where code outside the function can see it: before a call, an import, a
trap or a return the registers changed since the context was last in step are stored back, and after a call (which
may change any of them, or come back through longjmp with restored ones) all of them are loaded again; loads nobody
reads cost nothing, the compiler drops them. Which registers may have changed comes from a forward data-flow over the
function's lines (labels, gotos, the BR dispatch, before which the context is brought in step: what is dirty
there would otherwise flow into every label); a register outside that set holds what the context holds. The
SIMD registers and the flags stay in the context.

    localize(lines) -> (prologue, lines)     lines: the function body as recomp.lift_function writes it
Used by recomp.lift_function (RECOMP_REG_LOCALS=0: off) and by difftest.py, which runs every test through it.
"""
import os
import re

ENABLED = os.environ.get('RECOMP_REG_LOCALS', '1') != '0'
# ABI calls (RECOMP_REG_ABI=1): at a BL to a lifted function x19-x28 are not stored nor loaded again. The callee saves
# and restores whatever the context holds (AAPCS64: callee-saved), so a register in step before the call is in step
# after it, and one that is not stays right in the caller's local; only x0-x18 and x30 are loaded again. Imports,
# call_indirect (it may reach the host), tail calls and returns still bring the context fully in step (setjmp, the
# unwinder and the host read x19-x29 there).
ABI = os.environ.get('RECOMP_REG_ABI', '1') != '0'
_CALLEE_SAVED = sum(1 << n for n in range(19, 29))
_CLOBBERED = sum(1 << n for n in range(0, 19)) | 1 << 30
_LIFTED_CALL = re.compile(r'\bf_[0-9a-f]+\(c\);')
# check build: at every exit, the registers the analysis calls clean must hold what the context holds (guest_trap if not)
CHECK = os.environ.get('RECOMP_REG_LOCALS_CHECK', '0') == '1'

_REG = re.compile(r'c->x\[(\d+)\]')
_WRITE = re.compile(r'c->x\[(\d+)\] (?:[-+*/%&|^]|<<|>>)?=(?!=)')
_CALL = r'(?:(?:f_[0-9a-f]+|imp_\w+|h_\w+)\(c\);|call_indirect\(c, \w+\);|ic_call\(c, t, &ic_[0-9a-f]+, 0x[0-9a-f]+ULL\);|host_import\(c, \d+\);' \
        r'|guest_svc\(c, 0x[0-9a-f]+\);|guest_(?:trap|unimpl)\([^;]*\);)'
# everything that leaves the function or lets other code read the context; a following `return;` is a tail call
_EXIT = re.compile(_CALL + r'(?: return;)?|\breturn;|goto L_dispatch;')
# a line that is one call and nothing conditional (BL, BLR, an import): the context is in step after it
_PLAIN_CALL = re.compile(r'^(?:L_[0-9a-f]+: )?\s*(?:\{ (?:static uint64_t ic_[0-9a-f]+; )?uint64_t t = [^;]*; )?(?:c->x\[30\] = 0x[0-9a-f]+ULL; )?'
                         + _CALL + r'(?: \})?$')
_LABEL = re.compile(r'^(L_[0-9a-f]+|L_dispatch):')
_GOTO = re.compile(r'goto (L_[0-9a-f]+|L_dispatch);')
# the unconditional transfers (no way on to the next line); anything else, `if (...) goto` included, falls through
_ENDS = re.compile(r'^(?:L_[0-9a-f]+: )?\s*(?:goto L_\w+;|return;|\{ [^{}]*(?:return;|goto L_dispatch;) \}'
                   r'|\{ uint64_t jt = [^{}]*switch \(jt\) \{[^{}]*\} \})$')
# helpers that take the context but touch only the flags, the exclusive monitor or the SIMD registers (runtime.h, neon.h)
_CTX_OK = {'adds32', 'adds64', 'logic32', 'logic64', 'set_nzcv', 'cond', 'fcmp', 'stx8', 'stx16', 'stx32', 'stx64',
           'v_tbl', 'v_stn', 'call_indirect', 'ic_call', 'host_import', 'guest_svc'}
_CTX_ARG = re.compile(r'\b(\w+)\(c[,)]')
# NZCV: the AAPCS64 keeps no flags across a call or a return, so they live in a local `fl` (runtime.h `Nzcv`); the
# context gets them only where control goes on in another function without coming back (B or BR out of the function,
# the fall into the next listed function, a trap): if the function list split one real function, the flags still flow.
_FLAGS = re.compile(r'\b(adds32|adds64|logic32|logic64|set_nzcv|cond|fcmp)\(c, ')


def _name(n):
    return 'rx%d' % n


def _mask(nums):
    m = 0
    for n in nums:
        m |= 1 << int(n)
    return m


def _regs(mask):
    return [n for n in range(32) if mask >> n & 1]


def _check(line):
    for name in _CTX_ARG.findall(line):
        if name not in _CTX_OK and not re.match(r'(?:f_[0-9a-f]+|imp_\w+|h_\w+)$', name):
            raise ValueError('reglocal: %s(c, ...) may read the registers; list it in _EXIT or _CTX_OK' % name)


def _dirty_in(lines, writes):
    """Registers that may differ from the context on entry to each line (forward union over the line graph)."""
    n = len(lines)
    index = {}
    for i, s in enumerate(lines):
        m = _LABEL.match(s)
        if m:
            index[m.group(1)] = i
    every = [i for i, s in enumerate(lines) if _LABEL.match(s) and not s.startswith('L_dispatch')]
    succ = []
    for i, s in enumerate(lines):
        if s.startswith('L_dispatch'):
            nxt = every                                    # BR: any instruction of the function
        else:
            nxt = [index[t] for t in _GOTO.findall(s) if t in index]
            if i + 1 < n and not _ENDS.match(s):
                nxt.append(i + 1)
        succ.append(nxt)
    out_of = [(0 if _PLAIN_CALL.match(s) or 'goto L_dispatch;' in s else None) for s in lines]
    abi = [ABI and _PLAIN_CALL.match(s) is not None and _LIFTED_CALL.search(s) is not None for s in lines]
    din = [0] * n
    seen = [False] * n
    work = [0]
    seen[0] = True
    while work:
        i = work.pop()
        if abi[i]:
            out = (din[i] | writes[i]) & _CALLEE_SAVED          # what was out of step before the call still is
        else:
            out = out_of[i] if out_of[i] is not None else din[i] | writes[i]
        for j in succ[i]:
            if not seen[j] or out & ~din[j]:
                din[j] |= out
                seen[j] = True
                work.append(j)
    return din


def localize(lines, flags_at_return=False):
    """flags_at_return: the flags go back to the context at every return too (difftest compares them)."""
    used = _mask(n for s in lines for n in _REG.findall(s))
    flags = any(_FLAGS.search(s) for s in lines)
    if not used and not flags:
        return [], list(lines)
    put_flags = ' *NZCV_OF(c) = fl;' if flags else ''
    writes = [_mask(_WRITE.findall(s)) for s in lines]
    din = _dirty_in(lines, writes)
    load = ' '.join('%s = c->x[%d];' % (_name(n), n) for n in _regs(used))
    abi_load = ' '.join('%s = c->x[%d];' % (_name(n), n) for n in _regs(used & _CLOBBERED))
    out = []
    pc = 0
    for s, w, d in zip(lines, writes, din):
        _check(s)
        m = _LABEL.match(s)
        if m and m.group(1) != 'L_dispatch':
            pc = int(m.group(1)[2:], 16)
        check = ''.join(' if (%s != c->x[%d]) guest_trap("reglocal: x%d out of step", 0x%xULL);' % (_name(n), n, n, pc)
                        for n in _regs(used & ~(d | w))) if CHECK else ''
        store = ' '.join('c->x[%d] = %s;' % (n, _name(n)) for n in _regs(d | w)) + check
        abi_store = ' '.join('c->x[%d] = %s;' % (n, _name(n)) for n in _regs((d | w) & ~_CALLEE_SAVED)) + check

        def at_exit(m, store=store, abi_store=abi_store):
            text = m.group(0)
            if text == 'goto L_dispatch;':
                return '{ %s %s }' % (store, text) if store else text
            if text == 'return;':                         # back to the caller: no flags (unless difftest)
                put = store + (put_flags if flags_at_return else '')
                return '{ %s %s }' % (put, text) if put.strip() else text
            if text.endswith(' return;') or text.startswith('guest_'):   # on in another function, or a trap
                return '{ %s%s %s }' % (store, put_flags, text)
            if ABI and _LIFTED_CALL.fullmatch(text):          # a BL to lifted code: x19-x28 stay in the locals
                return '{ %s %s %s }' % (abi_store, text, abi_load)
            return '{ %s %s %s }' % (store, text, load) if store or load else text   # a call: flags not kept by it

        s = _REG.sub(lambda m: _name(int(m.group(1))), s)
        s = _FLAGS.sub(lambda m: m.group(1) + '_l(&fl, ', s)
        out.append(_EXIT.sub(at_exit, s))
    prologue = []
    if used:
        prologue.append('  uint64_t %s;' % ', '.join('%s = c->x[%d]' % (_name(n), n) for n in _regs(used)))
    if flags:
        prologue.append('  Nzcv fl = *NZCV_OF(c);')
    return prologue, out
