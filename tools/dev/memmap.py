"""Where a running game's memory goes: every allocation of the process with its committed and resident size, named
where possible.

    python tools/dev/memmap.py <pid | exe name> [--log <game log>] [--top 40] [--min-mb 1] [--json <file>]

Reads the process from outside and writes nothing into it: VirtualQueryEx (allocations, committed size), QueryWorkingSet
(which pages are in memory), each thread's TEB (stack range, start address -> module or runtime function from the exe's
.map), the PEB's heap list (NT heap segments and large blocks). --log names runtime allocations whose address the game
printed (the memory heap, virtual memory pools, Vulkan memory blocks with SMO_VK_MEMLOG=1).

Counters at the top: working set (all pages in memory, a page mapped at two addresses counts twice), private working
set, private bytes (commit of private memory) and the commit of pagefile-backed sections the process maps.
"""
import argparse
import bisect
import collections
import ctypes
import json
import os
import re
import sys
from ctypes import wintypes as wt

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
ntdll = ctypes.WinDLL('ntdll')

MEM_COMMIT, MEM_RESERVE, MEM_FREE = 0x1000, 0x2000, 0x10000
MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE = 0x20000, 0x40000, 0x1000000
PAGE = 4096


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_uint64), ('AllocationBase', ctypes.c_uint64), ('AllocationProtect', wt.DWORD),
                ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_uint64), ('State', wt.DWORD), ('Protect', wt.DWORD),
                ('Type', wt.DWORD)]


class PMC(ctypes.Structure):          # PROCESS_MEMORY_COUNTERS_EX2
    _fields_ = [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD)] + [(n, ctypes.c_size_t) for n in (
        'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage',
        'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage',
        'PrivateWorkingSetSize', 'SharedCommitUsage')]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD), ('th32OwnerProcessID', wt.DWORD),
                ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG), ('dwFlags', wt.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD), ('szExeFile', ctypes.c_wchar * 260)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
psapi.QueryWorkingSet.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD]
psapi.GetMappedFileNameW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_wchar_p, wt.DWORD]
psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD]


def find_pid(what):
    if what.isdigit():
        return int(what)
    snap = k32.CreateToolhelp32Snapshot(2, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    found = []
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == what.lower() or e.szExeFile.lower() == what.lower() + '.exe':
            found.append(e.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    if len(found) != 1:
        raise SystemExit('%d processes named %s' % (len(found), what))
    return found[0]


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, addr, buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return buf.raw


def u64(b, off=0):
    return int.from_bytes(b[off:off + 8], 'little')


def regions(h):
    """Every region of the address space that is not free: (base, size, state, protect, type, allocation base)."""
    out = []
    addr, m = 0, MBI()
    while addr < 0x7FFFFFFF0000:
        if not k32.VirtualQueryEx(h, addr, ctypes.byref(m), ctypes.sizeof(m)):
            break
        if m.State != MEM_FREE:
            out.append((m.BaseAddress, m.RegionSize, m.State, m.Protect, m.Type, m.AllocationBase))
        addr = m.BaseAddress + m.RegionSize
    return out


def working_set(h, guess):
    """Virtual page numbers in memory, and which of them are shareable."""
    n = guess + 65536
    while True:
        buf = (ctypes.c_uint64 * (n + 1))()
        if psapi.QueryWorkingSet(h, buf, ctypes.sizeof(buf)):
            cnt = buf[0]
            return [buf[i + 1] for i in range(cnt)]
        if ctypes.get_last_error() != 24:          # ERROR_BAD_LENGTH: buf[0] holds the count
            raise OSError('QueryWorkingSet failed: %d' % ctypes.get_last_error())
        n = buf[0] + 65536


def load_map(path):
    """(preferred base, sorted [(address, name)]) of the exe's linker map (tools/lifter/mapaddr.py)."""
    sys.path.insert(0, os.path.join(REPO, 'tools', 'lifter'))
    import mapaddr
    base, syms = mapaddr.load(path)
    return base, [(s[0], s[1]) for s in syms]


def threads(h, pid, modules, exe_map):
    """{stack allocation base: description} from each thread's TEB."""
    out = {}
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Thread32First(snap, ctypes.byref(e))
    tbi = (ctypes.c_uint64 * 6)()
    start = ctypes.c_uint64()
    while ok:
        if e.th32OwnerProcessID == pid:
            th = k32.OpenThread(0x0040 | 0x0800, False, e.th32ThreadID)      # QUERY_INFORMATION | QUERY_LIMITED
            if th:
                ntdll.NtQueryInformationThread(wt.HANDLE(th), 0, tbi, ctypes.sizeof(tbi), None)
                ntdll.NtQueryInformationThread(wt.HANDLE(th), 9, ctypes.byref(start), 8, None)
                teb = tbi[1]
                tb = read(h, teb, 0x1480) if teb else None
                if tb:
                    dealloc = u64(tb, 0x1478)
                    where = name_address(start.value, modules, exe_map)
                    out.setdefault(dealloc, []).append('thread %d stack (starts at %s)' % (e.th32ThreadID, where))
                k32.CloseHandle(wt.HANDLE(th))
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def name_address(a, modules, exe_map):
    for base, end, path in modules:
        if base <= a < end:
            mod = os.path.basename(path)
            if exe_map and path.lower().endswith('.exe'):
                pref, syms = exe_map
                rva = a - base + pref
                i = bisect.bisect_right(syms, (rva, '￿')) - 1
                if i >= 0:
                    return '%s!%s+0x%x' % (mod, syms[i][1], rva - syms[i][0])
            return '%s+0x%x' % (mod, a - base)
    return '0x%x' % a


def heaps(h):
    """{allocation base: label} of NT heap segments and large blocks (PEB.ProcessHeaps; _HEAP layout of Windows 10/11
    x64: signature 0xEEFFEEFF at +0x98, VirtualAllocdBlocks at +0x110, SegmentList at +0x120)."""
    out = {}
    pbi = (ctypes.c_uint64 * 6)()
    ntdll.NtQueryInformationProcess(wt.HANDLE(h), 0, pbi, ctypes.sizeof(pbi), None)
    peb = read(h, pbi[1], 0x100)
    if not peb:
        return out
    nheaps, plist = int.from_bytes(peb[0xE8:0xEC], 'little'), u64(peb, 0xF0)
    hl = read(h, plist, 8 * nheaps) or b''
    for i in range(nheaps):
        hb = u64(hl, 8 * i)
        hd = read(h, hb, 0x130)
        if not hd:
            continue
        if int.from_bytes(hd[0x98:0x9C], 'little') != 0xEEFFEEFF:
            out[hb] = 'heap %d (not an NT heap)' % i
            continue
        out[hb] = 'heap %d segment' % i
        for head, what in ((hb + 0x110, 'large block'), (hb + 0x120, 'segment')):
            e, seen = u64(hd, head - hb), 0
            while e and e != head and seen < 100000:
                if what == 'large block':
                    out[e] = 'heap %d large block' % i
                else:
                    out[e - 0x18] = 'heap %d segment' % i
                nxt = read(h, e, 8)
                e = u64(nxt) if nxt else 0
                seen += 1
    return out


def log_labels(path):
    """{address: label} from the game's log."""
    out = {}
    if not path:
        return out
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'SetMemoryHeapSize\(0x([0-9a-f]+)\): ok, heap at ([0-9A-Fa-f]+)', line)
        if m:
            out[int(m.group(2), 16)] = 'guest memory heap (section view; heap size 0x%s)' % m.group(1)
        m = re.search(r'virtual memory pool (\w+): 0x([0-9a-f]+) bytes at ([0-9a-f]+)', line)
        if m:
            out[int(m.group(3), 16)] = 'NVN virtual memory pool %s (views of the heap section)' % m.group(1)
        m = re.search(r'nvn_vk_mem: block (\d+): (\d+) MB, type (\d+) \((.*?)\), class (\d+), mapped at ([0-9A-Fa-f]+)', line)
        if m and int(m.group(6), 16):
            out[int(m.group(6), 16)] = 'Vulkan memory block %s (type %s %s, class %s)' % (m.group(1), m.group(3), m.group(4), m.group(5))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('process')
    ap.add_argument('--log')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--min-mb', type=float, default=1.0)
    ap.add_argument('--json')
    ap.add_argument('--zero', type=float, default=0, metavar='MB',
                    help='for allocations with at least this many MB in memory: how many of those pages hold only zeros, '
                         'and where in the allocation the pages in memory are (64 MB bands); reads only pages in memory')
    a = ap.parse_args()
    pid = find_pid(a.process)
    h = k32.OpenProcess(0x0400 | 0x0010, False, pid)
    if not h:
        raise SystemExit('cannot open process %d (error %d)' % (pid, ctypes.get_last_error()))
    pmc = PMC()
    pmc.cb = ctypes.sizeof(pmc)
    if not psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
        pmc.cb = ctypes.sizeof(PMC) - 16
        psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    regs = regions(h)
    ws = working_set(h, pmc.WorkingSetSize // PAGE)
    # allocations: base -> dict
    allocs = collections.OrderedDict()
    for base, size, state, prot, typ, ab in regs:
        A = allocs.setdefault(ab, {'base': ab, 'type': typ, 'reserved': 0, 'committed': 0, 'resident': 0, 'shared': 0,
                                   'regions': 0, 'name': ''})
        A['reserved'] += size
        A['regions'] += 1
        if state == MEM_COMMIT:
            A['committed'] += size
    starts = [r[0] for r in regs]
    pages = collections.defaultdict(list)
    for e in ws:
        va = e & ~0xFFF
        i = bisect.bisect_right(starts, va) - 1
        if i < 0 or va >= regs[i][0] + regs[i][1]:
            continue
        A = allocs[regs[i][5]]
        A['resident'] += PAGE
        if e >> 8 & 1:
            A['shared'] += PAGE
        if a.zero:
            pages[A['base']].append(va)
    # names
    modules = []
    buf = ctypes.create_unicode_buffer(1024)
    for A in allocs.values():
        if A['type'] in (MEM_IMAGE, MEM_MAPPED) and psapi.GetMappedFileNameW(h, A['base'], buf, 1024):
            A['name'] = buf.value
            if A['type'] == MEM_IMAGE:
                modules.append((A['base'], A['base'] + A['reserved'], buf.value))
        elif A['type'] == MEM_MAPPED:
            A['name'] = 'pagefile-backed section view'
    exe = next((m for m in modules if m[2].lower().endswith('.exe')), None)
    exe_map = None
    if exe:
        # the exe's .map beside it: GetMappedFileName gives a device path (\Device\HarddiskVolumeN\...), so the folder is
        # found from the path below the volume
        tail = exe[2].split('\\', 3)[-1] if exe[2].startswith('\\Device\\') else exe[2]
        for drive in 'CDEFG':
            p = '%s:\\%s.map' % (drive, tail[:-4])
            if os.path.exists(p):
                exe_map = load_map(p)
                break
    labels = {}
    labels.update(heaps(h))
    for k, v in threads(h, pid, modules, exe_map).items():
        labels[k] = '; '.join(v)
    labels.update(log_labels(a.log))
    for A in allocs.values():
        if A['base'] in labels and not (A['type'] == MEM_IMAGE):
            A['name'] = labels[A['base']] + ('' if not A['name'] or A['name'] == 'pagefile-backed section view' else ' ' + A['name'])
    for base, lab in labels.items():          # labels of addresses inside an allocation (a view at an offset)
        if base not in allocs:
            i = bisect.bisect_right(starts, base) - 1
            if i >= 0 and base < regs[i][0] + regs[i][1]:
                A = allocs[regs[i][5]]
                if lab not in A['name']:
                    A['name'] = (A['name'] + '; ' if A['name'] else '') + 'contains ' + lab
    mb = lambda x: x / 2 ** 20
    print('process %d: working set %.0f MB (private %.0f MB), private bytes %.0f MB, shared commit %.0f MB, peak working set %.0f MB'
          % (pid, mb(pmc.WorkingSetSize), mb(pmc.PrivateWorkingSetSize), mb(pmc.PrivateUsage), mb(pmc.SharedCommitUsage),
             mb(pmc.PeakWorkingSetSize)))
    tot = collections.Counter()
    for A in allocs.values():
        t = {MEM_IMAGE: 'image', MEM_MAPPED: 'mapped', MEM_PRIVATE: 'private'}.get(A['type'], '?')
        tot[t + ' committed'] += A['committed']
        tot[t + ' resident'] += A['resident']
    print('by type: ' + ', '.join('%s %.0f MB' % (k, mb(v)) for k, v in sorted(tot.items())))
    print('working set entries: %d (%.0f MB)' % (len(ws), mb(len(ws) * PAGE)))
    rows = sorted(allocs.values(), key=lambda A: -A['resident'])
    print('%-14s %-7s %9s %9s %9s %9s  %s' % ('allocation', 'type', 'reserved', 'committed', 'resident', 'shared', 'name'))
    shown = 0
    rest = collections.Counter()
    for A in rows:
        if shown < a.top and mb(A['resident']) >= a.min_mb:
            t = {MEM_IMAGE: 'image', MEM_MAPPED: 'mapped', MEM_PRIVATE: 'private'}.get(A['type'], '?')
            print('%014x %-7s %9.1f %9.1f %9.1f %9.1f  %s' % (A['base'], t, mb(A['reserved']), mb(A['committed']), mb(A['resident']),
                                                         mb(A['shared']), A['name'][:150]))
            shown += 1
        else:
            rest['n'] += 1
            rest['committed'] += A['committed']
            rest['resident'] += A['resident']
    print('the other %d allocations: committed %.0f MB, resident %.0f MB' % (rest['n'], mb(rest['committed']), mb(rest['resident'])))
    big = sorted((A for A in allocs.values() if A['committed'] >= 16 << 20), key=lambda A: -A['committed'])
    print('\nallocations with 16 MB or more committed (by commit):')
    for A in big[:a.top]:
        t = {MEM_IMAGE: 'image', MEM_MAPPED: 'mapped', MEM_PRIVATE: 'private'}.get(A['type'], '?')
        print('%014x %-7s %9.1f %9.1f %9.1f  %s' % (A['base'], t, mb(A['reserved']), mb(A['committed']), mb(A['resident']), A['name'][:150]))
    if a.zero:
        print('\npages in memory that hold only zeros (allocations with %.0f MB or more in memory):' % a.zero)
        zpage = bytes(PAGE)
        for A in sorted(allocs.values(), key=lambda A: -A['resident']):
            if mb(A['resident']) < a.zero:
                break
            vas = sorted(pages[A['base']])
            zero = unread = 0
            bands = collections.Counter()
            i = 0
            while i < len(vas):           # contiguous runs of pages in memory, read 1 MB at a time
                j = i
                while j + 1 < len(vas) and vas[j + 1] == vas[j] + PAGE and j + 1 - i < 256:
                    j += 1
                b = read(h, vas[i], (j - i + 1) * PAGE)
                if b is None:
                    unread += j - i + 1
                else:
                    for k in range(j - i + 1):
                        if b[k * PAGE:(k + 1) * PAGE] == zpage:
                            zero += 1
                for k in range(i, j + 1):
                    bands[(vas[k] - A['base']) >> 26] += 1
                i = j + 1
            print('%014x %9.1f MB in memory: %9.1f MB only zeros (%.0f%%)%s  %s' % (
                A['base'], mb(A['resident']), mb(zero * PAGE), 100.0 * zero * PAGE / max(A['resident'], 1),
                ', %d pages not readable' % unread if unread else '', A['name'][:60]))
            if A['reserved'] >= 256 << 20:
                print('    MB in memory per 64 MB band: ' + ' '.join('%d:%.0f' % (k, mb(v * PAGE)) for k, v in sorted(bands.items())))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'pid': pid, 'counters': {n: getattr(pmc, n) for n, _ in PMC._fields_},
                       'allocations': list(allocs.values())}, f)


if __name__ == '__main__':
    main()
