"""Copy a save folder so that its game file continues in another stage: a test can start in a late kingdom without
playing there. The source folder is only read; the copy is written to a new folder (never point --out at your own save).

    python tools/dev/save_warp.py <save folder> --out <new folder> --stage BossRaidWorldHomeStage [--world 11]
                                  [--scenario 2] [--checkpoint obj98] [--visited] [--reached obj99 ...] [--file File1.bin]

The game file (FileN.bin) is a 16-byte header (CRC-32 of everything after its first 4 bytes, version, ..., size of the
data) and a little-endian BYML (version 3) dictionary. The copy gets StageName = the stage, WorldId = its world (from
SystemData/WorldList.szs in build/romfs when --world is not given) and an empty CheckpointName and PlayerStartIdForSave
(the start point the game continues at), so the game starts the stage at its default entrance. --checkpoint names a
checkpoint flag of the stage (its object id, `tools/dev/byml.py StageData/<Stage>Map.szs --grep CheckpointFlag`) to
start at instead: it becomes CheckpointName, the flag's start point (its PlayerRestartPos link, from the stage's map
in build/romfs) PlayerStartIdForSave, and, when the file does not have the flag yet, it takes a free entry of the
file's table of flags reached (Checkpoint); a flag placed in a zone is
named "<id>(<zone>[<the zone's id in the stage>])", the way the game names placements. --reached marks more flags of the
stage as reached the same way, without starting at them (a test that goes on to them later). A world's first visit starts
at the ship whatever the checkpoint is: --visited marks the world as visited before (GameProgressData.IsFirstTimeWorld).
--scenario sets the world's scenario (WorldInfo[world].ScenarioNo), which picks the objects a kingdom's stages place.
Strings that are not in the file's string table are inserted in sorted order (the indices of the strings after them
move up, every offset behind the table moves by the table's growth).
"""
import argparse
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402  (SMO_TARGET: build/ or build/<update version>/)
OFFSET_TYPES = (0xC0, 0xC1, 0xD4, 0xD5, 0xD6)          # node types whose value is an offset to the node's data


def strtab(d, off):
    n = struct.unpack_from('<I', d, off)[0] >> 8
    offs = struct.unpack_from('<%dI' % (n + 1), d, off + 4)
    return [d[off + offs[i]:d.index(b'\0', off + offs[i])] for i in range(n)], offs[n]


def build_strtab(strings):
    n = len(strings)
    head = 4 + 4 * (n + 1)
    offs, body = [], b''
    for s in strings:
        offs.append(head + len(body))
        body += s + b'\0'
    offs.append(head + len(body))
    t = struct.pack('<I', 0xC2 | (n << 8)) + struct.pack('<%dI' % (n + 1), *offs) + body
    return t + b'\0' * (-len(t) % 4)


def slots(d, root_off):
    """(position, type) of every value slot of every array and dictionary reachable from the root."""
    out, seen, todo = [], set(), [(d[root_off], root_off)]
    while todo:
        t, v = todo.pop()
        if t not in (0xC0, 0xC1) or v in seen:
            continue
        seen.add(v)
        n = struct.unpack_from('<I', d, v)[0] >> 8
        if t == 0xC0:
            types = d[v + 4:v + 4 + n]
            base = v + 4 + ((n + 3) & ~3)
            items = [(types[i], base + 4 * i) for i in range(n)]
        else:
            items = [(d[v + 4 + 8 * i + 3], v + 4 + 8 * i + 4) for i in range(n)]
        for typ, pos in items:
            out.append((pos, typ))
            todo.append((typ, struct.unpack_from('<I', d, pos)[0]))
    return out


def dict_entry(d, dict_off, keys, name):
    """Position of the value slot and the type of key `name` in the dictionary at dict_off."""
    n = struct.unpack_from('<I', d, dict_off)[0] >> 8
    for i in range(n):
        w, = struct.unpack_from('<I', d, dict_off + 4 + 8 * i)
        if keys[w & 0xffffff] == name:
            return dict_off + 4 + 8 * i + 4, w >> 24
    raise SystemExit('the save has no %s' % name.decode())


def array_items(d, arr_off):
    """(position, type) of the value slots of the array at arr_off."""
    n = struct.unpack_from('<I', d, arr_off)[0] >> 8
    types = d[arr_off + 4:arr_off + 4 + n]
    base = arr_off + 4 + ((n + 3) & ~3)
    return [(base + 4 * i, types[i]) for i in range(n)]


def set_value(d, pos, typ, typ_want, value, what):
    if typ != typ_want:
        raise SystemExit('%s has node type %02x, expected %02x' % (what, typ, typ_want))
    struct.pack_into('<I' if typ == 0xA0 else '<i', d, pos, value)


def world_of(stage):
    import sarc as S
    from byml import Byml, sarc_files
    p = T.build('romfs', 'SystemData', 'WorldList.szs')
    files = sarc_files(S.yaz0(open(p, 'rb').read()))
    for i, w in enumerate(Byml(files['WorldList.byml']).root()):
        if w.get('Name') == stage:
            return i
    raise SystemExit('%s is not the home stage of a world: give --world' % stage)


def restart_of(stage, flag):
    """The start point of checkpoint flag `flag` of the stage (the flag's PlayerRestartPos link), named like the flag: a
    placement's CommonId if it has one, else its Id, in a zone "<Id>(<zone>[<the zone's Id in the stage>])"."""
    import re
    import sarc as S
    from byml import Byml, sarc_files
    m = re.match(r'([^()]+)\(([^\[]+)\[([^\]]+)\]\)$', flag)
    fid, zone, zid = m.groups() if m else (flag, None, None)
    name = zone or stage
    files = sarc_files(S.yaz0(open(T.build('romfs', 'StageData', name + 'Map.szs'), 'rb').read()))
    scens = Byml(files[name + 'Map.byml']).root()
    for scen in scens if isinstance(scens, list) else [scens]:
        for o in scen.get('CheckPointList') or []:
            if isinstance(o, dict) and fid in (o.get('CommonId'), o.get('Id')):
                for p in (o.get('Links') or {}).get('PlayerRestartPos') or []:
                    if p.get('CommonId'):
                        return p['CommonId']
                    return '%s(%s[%s])' % (p.get('Id'), zone, zid) if zone else p.get('Id')
    raise SystemExit('%s: no checkpoint flag %s with a start point (PlayerRestartPos)' % (name, fid))


def warp(d, stage, world, scenario=None, checkpoint=None, visited=False, start='', reached=()):
    if d[:2] != b'YB' or struct.unpack_from('<H', d, 2)[0] not in (2, 3):
        raise SystemExit('not a little-endian BYML version 2 or 3')
    key_off, str_off, root_off = struct.unpack_from('<III', d, 4)
    if not key_off < str_off < root_off:
        raise SystemExit('unexpected BYML layout (key table, string table, data)')
    keys, _ = strtab(d, key_off)
    old, end_rel = strtab(d, str_off)
    old_end = (str_off + end_rel + 3) & ~3
    new, flag, start = stage.encode(), (checkpoint or '').encode(), start.encode()
    flags = [f.encode() for f in ([checkpoint] if checkpoint else []) + list(reached)]
    strings = sorted(set(old) | {new, flag, start} | set(flags))
    remap = [strings.index(s) for s in old]
    table = build_strtab(strings)
    delta = str_off + len(table) - old_end
    sl = slots(d, root_off)
    out = bytearray(d[:str_off] + table + d[old_end:])
    struct.pack_into('<I', out, 12, root_off + delta)

    def fix(v):
        return v + delta if v >= old_end else v
    for pos, typ in sl:
        p = pos + delta
        v, = struct.unpack_from('<I', out, p)
        if typ == 0xA0:
            struct.pack_into('<I', out, p, remap[v])
        elif typ in OFFSET_TYPES:
            struct.pack_into('<I', out, p, fix(v))
    root = root_off + delta
    for name, typ_want, value in ((b'StageName', 0xA0, strings.index(new)), (b'CheckpointName', 0xA0, strings.index(flag)),
                                  (b'PlayerStartIdForSave', 0xA0, strings.index(start)), (b'WorldId', 0xD1, world)):
        pos, typ = dict_entry(out, root, keys, name)
        set_value(out, pos, typ, typ_want, value, name.decode())
    if scenario is not None:
        pos, typ = dict_entry(out, root, keys, b'WorldInfo')
        items = array_items(out, struct.unpack_from('<I', out, pos)[0])
        if typ != 0xC0 or world >= len(items) or items[world][1] != 0xC1:
            raise SystemExit('WorldInfo is not an array of dictionaries with an entry for world %d' % world)
        pos, typ = dict_entry(out, struct.unpack_from('<I', out, items[world][0])[0], keys, b'ScenarioNo')
        set_value(out, pos, typ, 0xD1, scenario, 'WorldInfo[%d].ScenarioNo' % world)
    if visited:                                         # a first visit starts at the ship whatever the checkpoint says
        pos, typ = dict_entry(out, root, keys, b'GameProgressData')
        pos, typ = dict_entry(out, struct.unpack_from('<I', out, pos)[0], keys, b'IsFirstTimeWorld')
        items = array_items(out, struct.unpack_from('<I', out, pos)[0])
        if typ != 0xC0 or world >= len(items):
            raise SystemExit('GameProgressData.IsFirstTimeWorld has no entry for world %d' % world)
        set_value(out, items[world][0], items[world][1], 0xD0, 0, 'IsFirstTimeWorld[%d]' % world)
    if flags:                                           # a flag to start at must be one the file has reached: a free entry takes it
        pos, typ = dict_entry(out, root, keys, b'Checkpoint')
        entries = []
        for p, t in array_items(out, struct.unpack_from('<I', out, pos)[0]):
            e = struct.unpack_from('<I', out, p)[0]
            o, ot = dict_entry(out, e, keys, b'ObjId')
            s, st = dict_entry(out, e, keys, b'StageName')
            entries.append([struct.unpack_from('<I', out, o)[0], struct.unpack_from('<I', out, s)[0], o, s])
        for f in flags:
            if any(ob == strings.index(f) and sn == strings.index(new) for ob, sn, _, _ in entries):
                continue
            free = next((e for e in entries if e[0] == strings.index(b'') and e[1] == strings.index(b'')), None)
            if free is None:
                raise SystemExit('the table of checkpoint flags reached (Checkpoint) has no free entry')
            set_value(out, free[2], 0xA0, 0xA0, strings.index(f), 'Checkpoint ObjId')
            set_value(out, free[3], 0xA0, 0xA0, strings.index(new), 'Checkpoint StageName')
            free[0], free[1] = strings.index(f), strings.index(new)
    return bytes(out), len(sl)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('save', help='folder holding the save (Common.bin, FileN.bin)')
    ap.add_argument('--out', required=True, help='new folder for the copy (must not exist)')
    ap.add_argument('--stage', required=True)
    ap.add_argument('--world', type=int)
    ap.add_argument('--scenario', type=int, help="the world's scenario (WorldInfo[world].ScenarioNo)")
    ap.add_argument('--checkpoint', help='object id of a checkpoint flag of the stage to start at')
    ap.add_argument('--visited', action='store_true', help='the world counts as visited before (no first-visit arrival)')
    ap.add_argument('--reached', action='append', default=[], metavar='FLAG',
                    help='another checkpoint flag of the stage that counts as reached (the start point stays)')
    ap.add_argument('--file', help='game file to change (default: the only FileN.bin)')
    a = ap.parse_args()
    if os.path.exists(a.out):
        sys.exit('%s exists: give a new folder' % a.out)
    if os.path.abspath(a.out) == os.path.abspath(a.save):
        sys.exit('--out must be a new folder')
    name = a.file or next(n for n in sorted(os.listdir(a.save)) if n.startswith('File') and n.endswith('.bin'))
    world = a.world if a.world is not None else world_of(a.stage)
    raw = open(os.path.join(a.save, name), 'rb').read()
    crc, ver, unk, size = struct.unpack_from('<IIII', raw, 0)
    if zlib.crc32(raw[4:]) & 0xffffffff != crc:
        sys.exit('%s: the CRC-32 in the header does not match: not a game file of the kind this tool knows' % name)
    start = restart_of(a.stage, a.checkpoint) if a.checkpoint else ''
    data, nslots = warp(raw[16:16 + size], a.stage, world, a.scenario, a.checkpoint, a.visited, start, a.reached)
    body = struct.pack('<III', ver, unk, len(data)) + data
    if len(body) + 4 > len(raw):
        sys.exit('the new data does not fit the file')
    body += b'\0' * (len(raw) - 4 - len(body))
    shutil.copytree(a.save, a.out)
    with open(os.path.join(a.out, name), 'wb') as f:
        f.write(struct.pack('<I', zlib.crc32(body) & 0xffffffff) + body)
    print('%s: %s -> stage %s, world %d%s%s%s%s (%d value slots checked, data %d -> %d bytes)' % (
        a.out, name, a.stage, world, '' if a.scenario is None else ', scenario %d' % a.scenario,
        ', checkpoint %s (start point %s)' % (a.checkpoint, start) if a.checkpoint else '', ', visited before' if a.visited else '',
        ', also reached: %s' % ' '.join(a.reached) if a.reached else '', nslots, size, len(data)))


if __name__ == '__main__':
    main()
