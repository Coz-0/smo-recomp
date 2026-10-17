"""Compare the draws of one frame with the frames before and after it (find what a flash frame draws differently).

    python tools/dev/drawdiff.py build/run/<name>.log <frame> [--fields all|state]

The log must come from a run with SMO_VK_PASSSHOT=<first>-<last> (SMO_VK_PASSSHOT_LOGONLY=1 skips the pictures) and
SMO_VK_FRAMELOG=<first>-<last> over the same frames: the draw lines of each frame end with its "nvn_vk_frame:" line.
Each draw is reduced to what does not change with the camera (programs, render target and pass size, viewport,
scissor, blend / mask / depth state, the textures bound and the vertex buffers' strides), the three frames are aligned
by that, and the draws of the middle frame that match neither neighbour while the neighbours match each other are
printed with their full log lines.
"""
import argparse
import difflib
import re


def frames_of(path):
    frames, cur, draw = {}, [], None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('nvn_vk_frame: '):
            n = int(line.split()[1])
            if draw:
                cur.append(draw)
            frames[n] = cur
            cur, draw = [], None
        elif line.startswith('nvn_vk_draw: prim'):
            if draw:
                cur.append(draw)
            draw = [line]
        elif draw is not None and (line.startswith('nvn_vk_draw:   ') or line.startswith('nvn_vk_bind:') or line.startswith('nvn_vk_vtx:')):
            draw.append(line)
    return frames


def key(draw):
    """What a draw is, without what moves with the camera."""
    head = draw[0]
    head = re.sub(r'\| ds [0-9a-f]+', '', head)
    parts = [head]
    for line in draw[1:]:
        if line.startswith('nvn_vk_draw:   depth target'):
            parts.append(re.sub(r'target [0-9A-F]+ ', 'target ', line.split(' drange')[0]))
        elif line.startswith('nvn_vk_bind: stage'):
            m = re.search(r'stage (\d+) unit (\d+) handle [0-9a-f]+ -> (\w+) tex [0-9A-F]+ fmt (\w+) (\d+x\d+)', line)      # handles rotate from frame to frame
            parts.append('bind %s' % ' '.join(m.groups()) if m else line[:80])
        elif line.startswith('nvn_vk_vtx:'):
            m = re.search(r'attr (\d+) fmt (\w+) stream (\d+) offset (\d+) stride (\d+)', line)
            if m:
                parts.append('vtx %s' % ' '.join(m.groups()))
    return '\n'.join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('frame', type=int)
    a = ap.parse_args()
    frames = frames_of(a.log)
    f = a.frame
    if not all(k in frames for k in (f - 1, f, f + 1)):
        raise SystemExit('frames %d..%d are not all in the log (have %s..%s)' % (f - 1, f + 1, min(frames, default=None), max(frames, default=None)))
    p, c, n = ([key(d) for d in frames[k]] for k in (f - 1, f, f + 1))
    print('draws: frame %d: %d, frame %d: %d, frame %d: %d' % (f - 1, len(p), f, len(c), f + 1, len(n)))
    sp, sn = set(p), set(n)
    odd = [i for i, k in enumerate(c) if k not in sp and k not in sn]
    print('draws of frame %d that appear in neither neighbour: %d' % (f, len(odd)))
    for i in odd[:20]:
        print('--- draw %d of frame %d:' % (i, f))
        print('\n'.join('    ' + l[:260] for l in frames[f][i]))
    sm = difflib.SequenceMatcher(None, p, n, autojunk=False)
    missing = [i for i, k in enumerate(p) if k in sn and k not in set(c)]
    print('draws in both neighbours but missing from frame %d: %d' % (f, len(missing)))
    for i in missing[:10]:
        print('--- draw %d of frame %d (missing in %d):' % (i, f - 1, f))
        print('\n'.join('    ' + l[:260] for l in frames[f - 1][i]))
    print('neighbours %d and %d share %.1f%% of their draws' % (f - 1, f + 1, 100.0 * sm.ratio()))


if __name__ == '__main__':
    main()
