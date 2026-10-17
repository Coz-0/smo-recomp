"""Background CPU load and a CPU-use log, to measure the frame rate while other programs are busy.

    python tools/dev/cpuload.py [--busy N] [--seconds S] [--log FILE] [--every 5]

--busy N    N worker processes at normal priority, each busy all the time: bursts of arithmetic with a 4 MB copy
            between them (about 1 GB/s of memory traffic per worker), as a browser or a build would be. 0 = no load.
--seconds S stop after S seconds (default: until killed).
--log FILE  every --every seconds one line (to FILE, else to the console): seconds since the start, the CPU use of the
            whole system in cores, the game's (smo_*.exe), the load's and the four busiest other processes, in % of one core.
Windows only (the log reads process times with the Win32 API).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import multiprocessing
import os
import sys
import time


def worker(seed):
    src = bytearray(os.urandom(1 << 16)) * 64           # 4 MB
    dst = bytearray(len(src))
    x = seed
    while True:
        for _ in range(40000):                           # a few milliseconds of arithmetic
            x = (x * 1103515245 + 12345) & 0x7fffffff
        dst[:] = src                                     # a 4 MB copy
        src[x & 0xffff] = x & 0xff


k32 = ctypes.WinDLL('kernel32', use_last_error=True) if os.name == 'nt' else None
psapi = ctypes.WinDLL('psapi', use_last_error=True) if os.name == 'nt' else None


def ft(f):
    return (f.dwHighDateTime << 32) | f.dwLowDateTime


def system_busy():
    idle, kern, user = wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
    k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user))
    return ft(kern) + ft(user) - ft(idle)                # 100 ns units summed over all logical CPUs


def processes():
    """{pid: (name, cpu time in 100 ns)}"""
    pids = (wt.DWORD * 8192)()
    got = wt.DWORD()
    psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got))
    out = {}
    for pid in pids[:got.value // 4]:
        h = k32.OpenProcess(0x1000, False, pid)          # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            continue
        try:
            c, e, kt, ut = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
            if not k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut)):
                continue
            name = ctypes.create_unicode_buffer(520)
            n = wt.DWORD(520)
            k32.QueryFullProcessImageNameW(h, 0, name, ctypes.byref(n))
            out[pid] = (os.path.basename(name.value).lower().replace('.exe', '') or '?', ft(kt) + ft(ut))
        finally:
            k32.CloseHandle(h)
    return out


def log_loop(path, every, until, own):
    fp = open(path, 'w', buffering=1) if path else None
    t0 = time.time()
    s0, p0, w0 = system_busy(), processes(), time.time()
    while until is None or time.time() < until:
        time.sleep(every)
        s1, p1, w1 = system_busy(), processes(), time.time()
        span = (w1 - w0) * 1e7
        use = {}
        for pid, (name, cpu) in p1.items():
            if pid in p0 and p0[pid][0] == name:
                use[pid] = (name, 100.0 * (cpu - p0[pid][1]) / span)
        game = sum(u for name, u in use.values() if name.startswith('smo_'))
        load = sum(u for pid, (name, u) in use.items() if pid in own)
        others = sorted(((u, name) for pid, (name, u) in use.items()
                         if not name.startswith('smo_') and pid not in own and name not in ('idle', 'system idle process')), reverse=True)[:4]
        line = 't+%.0fs system %.2f cores | game %.0f%% | load %.0f%% | %s' % (
            w1 - t0, (s1 - s0) / span, game, load, ', '.join('%s %.0f' % (n, u) for u, n in others))
        if fp:
            fp.write(line + '\n')
        else:
            print(line, flush=True)
        s0, p0, w0 = s1, p1, w1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--busy', type=int, default=0)
    ap.add_argument('--seconds', type=float)
    ap.add_argument('--log')
    ap.add_argument('--every', type=float, default=5.0)
    a = ap.parse_args()
    until = time.time() + a.seconds if a.seconds else None
    procs = [multiprocessing.Process(target=worker, args=(i + 1,), daemon=True) for i in range(a.busy)]
    for p in procs:
        p.start()
    own = {p.pid for p in procs}
    try:
        log_loop(a.log, a.every, until, own)
    except KeyboardInterrupt:
        pass
    finally:
        for p in procs:
            p.terminate()


if __name__ == '__main__':
    multiprocessing.freeze_support()
    main()
