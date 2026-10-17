"""Table per frame of a timeline written by SMO_FRAMETL (runtime/nvn_gl.c).

    python tools/dev/frametl.py <frametl file> [--slow 17.5] [--all]

SMO's frame, as the timeline shows it: at each refresh the game's present thread submits the frame recorded before it
(one batch), then presents; the render thread replays the batch and presents. Meanwhile the main thread updates the
next frame, waits until that present is done, draws the next frame, and waits for the next refresh (SyncWait on the
window texture). Per frame (times in ms from the refresh that started it):
  gap      time since the previous frame's present ended on the render thread (what the frame-time statistics measure)
  post     the batch was submitted
  replay   the render thread replaying the batch;  pres: its present (the blit, the swapchain, the occlusion results)
  gpuw     of the replay, waiting for the GPU (a submission still running);  presw: the same during the present
  done     the present returned to the game
  draw     present returned -> the main thread's wait for the next refresh began (its drawing, if it was waiting)
  margin   that wait began this long before the next refresh (negative: the refresh was missed)
  target   refreshes from this one to the one the main thread then waited for (1 = on time)
The slow frames (gap over --slow ms) are listed, then the averages of all frames and of the slow ones.
"""
import argparse
import bisect
import collections

TL_POST, TL_RUN, TL_RUN_RET, TL_JOB_BEGIN, TL_JOB_END, TL_PACE_BEGIN, TL_PACE_END, TL_VSYNC, TL_FRAME_END, TL_PRESENT_CALL, TL_SYNC_CALL, TL_SYNC_RET, \
    TL_GPU_WAIT, TL_GPU_WAIT_END, TL_GPU_SUBMIT = range(1, 16)       # as the enum in runtime/nvn_gl.c


def gpu_wait(ev, rt, i0, i1):
    """time the render thread waited for the GPU between events i0 and i1 (counter ticks)"""
    w, t0 = 0, None
    for t, tid, e, x in ev[i0:i1]:
        if tid != rt:
            continue
        if e == TL_GPU_WAIT:
            t0 = t
        elif e == TL_GPU_WAIT_END and t0 is not None:
            w += t - t0
            t0 = None
    return w


def load(path):
    """(counter Hz, render thread id, events (t, thread, event, arg) in time order, {thread: Counter of processors})"""
    head = open(path).readline()
    hz = float(head.split('counter ')[1].split(' Hz')[0])
    rt = int(head.split('render thread ')[1])
    ev, cpus = [], collections.defaultdict(collections.Counter)
    for l in open(path):
        if l.startswith('#'):
            continue
        f = list(map(int, l.split()))
        ev.append(tuple(f[:4]))
        if len(f) > 4:
            cpus[f[1]][f[4]] += 1
    ev.sort()
    return hz, rt, ev, cpus


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--slow', type=float, default=17.5)
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    hz, rt, ev, cpus = load(a.file)
    ms = lambda d: d * 1000.0 / hz
    vs = [(t, x) for t, tid, e, x in ev if e == TL_VSYNC]
    vt = [t for t, x in vs]
    rows = []
    i, n = 0, len(ev)
    while i < n:
        t, tid, e, x = ev[i]
        if not (tid == rt and e == TL_JOB_BEGIN and x == 0):
            i += 1
            continue
        r = {'rb': t}
        j = i + 1
        while j < n and not (ev[j][1] == rt and ev[j][2] == TL_JOB_END and ev[j][3] == 0):
            j += 1
        if j >= n:
            break
        r['re'] = ev[j][0]
        k = j + 1                                   # the present job after the batch (fence and flush come between)
        while k < n and not (ev[k][1] == rt and ev[k][2] in (TL_JOB_BEGIN, TL_JOB_END) and ev[k][3] in (0, 3)):
            k += 1
        if k >= n or ev[k][3] != 3 or ev[k][2] != TL_JOB_BEGIN:
            i = j + 1
            continue
        r['pb'] = ev[k][0]
        m = k + 1
        while m < n and not (ev[m][2] == TL_FRAME_END):
            m += 1
        q = k + 1
        while q < n and not (ev[q][1] == rt and ev[q][2] == TL_JOB_END and ev[q][3] == 3):
            q += 1
        if m >= n or q >= n:
            break
        r['fe'], r['frame'], r['pe'] = ev[m][0], ev[m][3], ev[q][0]
        r['gw'], r['pw'] = gpu_wait(ev, rt, i, j + 1), gpu_wait(ev, rt, k, q + 1)
        post = [ev[p][0] for p in range(max(0, i - 40), i) if ev[p][2] in (TL_POST, TL_RUN) and ev[p][3] == 0]
        r['post'] = post[-1] if post else t
        ret = [ev[p][0] for p in range(q, min(n, q + 40)) if ev[p][2] == TL_RUN_RET and ev[p][3] == 3]
        r['ret'] = ret[0] if ret else r['pe']
        sc = [(ev[p][0], ev[p][1]) for p in range(q, min(n, q + 200)) if ev[p][2] == TL_SYNC_CALL and ev[p][3] == 1]
        pb = [(ev[p][0], ev[p][3]) for p in range(q, min(n, q + 200)) if ev[p][2] == TL_PACE_BEGIN]
        r['sc'] = sc[0][0] if sc else None
        r['latch'] = pb[0][1] if pb else None
        vi = bisect.bisect_right(vt, r['post']) - 1     # the refresh that started this frame
        r['v'] = vt[vi] if vi >= 0 else r['post']
        r['vn'] = vs[vi][1] if vi >= 0 else 0
        r['vnext'] = vt[vi + 1] if vi + 1 < len(vt) else None
        rows.append(r)
        i = q + 1
    out = []
    for p, r in zip(rows, rows[1:]):
        d = {'frame': r['frame'], 'gap': ms(r['fe'] - p['fe']), 'post': ms(r['post'] - r['v']), 'replay': ms(r['re'] - r['rb']),
             'gpuw': ms(r['gw']), 'pres': ms(r['pe'] - r['pb']), 'presw': ms(r['pw']), 'done': ms(r['ret'] - r['v']), 'draw': ms(r['sc'] - r['ret']) if r['sc'] else 0,
             'margin': ms(r['vnext'] - r['sc']) if r['sc'] and r['vnext'] else 0, 'target': ((r['latch'] - r['vn']) & 0xffff) if r['latch'] is not None else 0}
        out.append(d)
    cols = ['gap', 'post', 'replay', 'gpuw', 'pres', 'presw', 'done', 'draw', 'margin', 'target']
    print('%6s ' % 'frame' + ' '.join('%7s' % c for c in cols))
    slow = [d for d in out if d['gap'] > a.slow]
    for d in out if a.all else slow:
        print('%6d ' % d['frame'] + ' '.join('%7.2f' % d[c] for c in cols))
    avg = lambda rs, c: sum(x[c] for x in rs) / len(rs) if rs else 0
    print('all  %5d ' % len(out) + ' '.join('%7.2f' % avg(out, c) for c in cols))
    print('slow %5d ' % len(slow) + ' '.join('%7.2f' % avg(slow, c) for c in cols))
    for c in ('replay', 'pres', 'done', 'margin'):
        v = sorted(x[c] for x in out)
        if v:
            print('%-7s p50 %6.2f p90 %6.2f p99 %6.2f max %6.2f min %6.2f' % (c, v[len(v) // 2], v[int(len(v) * 0.9)], v[int(len(v) * 0.99)], v[-1], v[0]))
    print('frames that waited for a later refresh than the next one: %d of %d; margin under 1 ms: %d' % (
        sum(1 for x in out if x['target'] > 1), len(out), sum(1 for x in out if x['margin'] < 1.0)))
    role = {rt: 'render'}
    for t, tid, e, x in ev:
        if tid != rt and e in (TL_PACE_BEGIN, TL_PRESENT_CALL, TL_VSYNC):
            role[tid] = {TL_PACE_BEGIN: 'main', TL_PRESENT_CALL: 'present', TL_VSYNC: 'refresh clock'}[e]
    for tid, c in sorted(cpus.items(), key=lambda kv: -sum(kv[1].values())):
        if role.get(tid):
            print('processors of the %s thread (events): %s' % (role[tid], ', '.join('%d:%d' % kv for kv in c.most_common(8))))


if __name__ == '__main__':
    main()
