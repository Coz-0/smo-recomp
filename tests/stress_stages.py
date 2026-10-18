"""Stress test: every stage of the game, started from a copy of a save warped there, played briefly at a raised speed.

    python tests/stress_stages.py --save <save folder> [--target 1.4.1] [--stages all | <name,name,...> | @<file>]
                                  [--scenarios] [--jobs 2] [--speed 4] [--seconds 150] [--cache <shader cache to copy>]
                                  [--chain N] [--direct] [--name <run>]

The stages (SystemData/WorldList.szs: WorldListFromDb, every stage of every world, with its world and category, then
the stages of StageData it does not list) start at their default entrance in the scenario the save has; the ones the
game does not play as stage scenes (cutscenes, the Labo VR mode's, the world map: not_a_stage_scene) are left out
unless --special asks for them. --scenarios
adds every scenario of every kingdom (WorldListFromDb ScenarioNum): its home stage in that scenario, started at one of
the scenario's checkpoint flags (the next one for each scenario), so a kingdom's scenarios also show different parts of
it. For each:
  1. tools/dev/save_warp.py makes a copy of the save that continues there (a save always resumes in a kingdom's home
     stage: for any other stage, in its kingdom's, and SMO_START_STAGE, TEST ONLY, makes the first stage scene the
     game starts that stage, at its default start point);
  2. scripts/run.py runs the game hidden with SMO_SPEED (TEST ONLY: the refresh clock runs that many times faster) and an
     input sequence: resume from the title menu, press A now and then until the stage plays (a talk that opens a
     scenario goes on), then wait, turn the camera round, look up and down, jump, throw the cap, wait again (in a
     kingdom's main stage also open and close the map and the pause menu);
  3. the log is read for problems: host exceptions, the game's aborts and failed assertions, renderer operations not
     implemented, shader stages not translated or rejected by glslang, Vulkan call failures, system calls reached
     without an implementation, freeze reports, dummy textures, audio dropped, the stage not reaching play in time;
     four small pictures are kept. Frame times are recorded as information only: at the raised speed, with jobs side by
     side, they are not those of normal play.
Everything goes to build/<version>/stress/<name>/: results.jsonl (one line per run), summary.md, thumbs/. Each job has
its own exe copy and shader cache copy; saves and full-size pictures are deleted as it goes (the disk fills quickly).
Exit status 0 when it ran; the summary lists what was found. The save folder is only read.

--chain N plays up to N stages in one run of the game, so that the game boots once for them (booting to the title menu
is about half of a run): stages of one kingdom and one kind (stages in the save's scenario; scenarios started at a
checkpoint flag; scenarios at their default start), in the order a job takes them. The first starts as above; each next
one is changed to by the game itself when the stage before has played its input sequence (SMO_STAGE_LIST, TEST ONLY:
a ChangeStageInfo given to GameDataFunction::tryChangeNextStage, as a door does, with the scenario and the checkpoint
flag's start point; the run's save has the flags of all of them reached), and its input sequence starts at its stage
change, not at the title menu. The log is split per stage and every stage gets its line in results.jsonl as its own run
would, its counters taken from the totals the game prints after each stage; the time limit is per stage. After a crash
or a time limit a new run plays the stages left. Messages the runtime prints once per run (a shader stage not
translated, the kinds of operations not implemented) appear only with the first stage of a run that meets them.

A stage that only a painting leads to (the Mushroom Kingdom's boss rematches, the Darker Side's last room;
painting_entry, from the maps) is entered as in play, whatever --chain is: a run starts in the painting's room (as
above), plays until play begins there, then changes to the stage the way the painting does (its ChangeStageName and
ChangeStageId, SMO_STAGE_LIST). Its line in results.jsonl is the stage's (entered_from names the room), its log the
whole run. Started directly, such a stage meets a restart after a miss in that room without the room's archives loaded,
which play never does (the restart then ran out of its 512 KB scene resource heap: nn::diag abort).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
TITLE = '@until-nerve:MenuSelectPartsNrvSelect:Z/30 60:Z'              # resume from the title menu
PLAY = ('@until-nerve:StageSceneNrvPlay:Z/45 300 1:6/90 30 1:4/180 30 1:6/90 60 1:8/40 60 1:2/80 60 1:8/40 120 1:Z/8 120 '
        '1:V/8 600')                                                    # A every 45 polls until play: a scenario's opening talk goes on
MAIN = ' 1:-/8 300 1:-/8 300 1:!/8 200 1:X/8 600'                       # a kingdom's main stage: the map and the pause menu too
SEQ = TITLE + ' ' + PLAY
SEQ_MAIN = SEQ + MAIN
SEQ_DEMO = TITLE + ' 3000'                                              # a scene stage: no play to wait for, it is watched
SHOTS_AFTER_PLAY = (150, 700, 1700, 2500)              # frames after play began: the stage, the camera turned, the map, the end
# a run of several stages (--chain, SMO_STAGE_LIST): the line that ends each stage, with the counters of the run so far
TOTALS = re.compile(r'stage list: stage \d+ of \d+ done at frame (\d+) \(poll \d+\): run totals: (\d+) frames presented, (\d+) Vulkan call '
                    r'failures, (\d+) calls of operations that are not implemented, imports reached (\d+) \(still stubs (\d+)\), audio device '
                    r'sink (\d+) frames, (\d+) samples dropped, (\d+) underruns, loading-screen holds (\d+) \((\d+) ms\), samplers given a '
                    r'dummy (\d+), process memory (\d+) MB, run time ([\d.]+) s')
TOTAL_KEYS = ('frame', 'frames', 'vk_failures', 'not_implemented', 'imports_reached', 'imports_stub', 'audio_frames', 'audio_dropped',
              'audio_underruns', 'load_holds', 'load_hold_ms', 'dummies', 'memory_mb', 'run_s')
STAGE_DONE = re.compile(r'stage list: stage \d+ of \d+ done at frame \d+[^\n]*\n?')
ABORTS = (r'(nn::diag abort[^\n]*|nn::diag assertion failed[^\n]*|\nassertion failed[^\n]*|nvn_vk: FATAL[^\n]*|'
          r'sdklift: lifted SDK code[^\n]*reached[^\n]*|C\+\+ RUNTIME:[^\n]*)')


def sequence(stage, first=True):
    """The input sequence of a stage; one after the first of a run of several (--chain) begins at its stage change, not
    at the title menu. The room a painting leads from (painting_entry) is only played until the stage can change."""
    if stage.get('lead_in'):
        return TITLE + ' @until-nerve:StageSceneNrvPlay:Z/45 60'
    if is_demo(stage):
        return SEQ_DEMO
    s = (TITLE + ' ' if first else '') + PLAY
    return s + MAIN if stage['category'] == 'MainStage' else s


def stage_list(target, zones=False, default_world=0):
    """Every stage of WorldListFromDb with a map file (zones, which are placed inside stages, only with zones=True), then
    the stages of StageData the list does not name (the ship's inside, the world map, travel scenes, the update's *Rsv*
    stages): their world from their name where it starts with a world's, else default_world."""
    os.environ['SMO_TARGET'] = target
    sys.path.insert(0, os.path.join(REPO, 'tools', 'dev'))
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    import target as T
    import sarc as S
    from byml import Byml, sarc_files
    files = sarc_files(S.yaz0(open(T.build('romfs', 'SystemData', 'WorldList.szs'), 'rb').read()))
    worlds = Byml(files['WorldListFromDb.byml']).root()
    out, seen = [], set()
    for world, w in enumerate(worlds):
        for s in w.get('StageList', []):
            name = s.get('name')
            if name and name not in seen and os.path.exists(T.build('romfs', 'StageData', name + 'Map.szs')):
                seen.add(name)
                if s.get('category') != 'Zone' or zones:
                    out.append({'id': name, 'stage': name, 'world': world, 'world_name': w.get('WorldName'), 'category': s.get('category')})
    prefixes = [(w.get('Name', '').replace('HomeStage', ''), i, w.get('WorldName')) for i, w in enumerate(worlds)]
    for n in sorted(os.listdir(T.build('romfs', 'StageData'))):
        name = n[:-len('Map.szs')] if n.endswith('Map.szs') else None
        if not name or not name.endswith('Stage') or name in seen:
            continue
        world = next(((i, wn) for p, i, wn in prefixes if p and name.startswith(p)), (default_world, worlds[default_world].get('WorldName')))
        out.append({'id': name, 'stage': name, 'world': world[0], 'world_name': world[1], 'category': 'NotInWorldList'})
    return out, worlds, T


def scenario_list(T, worlds):
    """Every scenario of every world (the count the game reads: WorldList.byml ScenarioNum): its home stage, started at
    the scenario's next checkpoint flag (a map's entry scenario - 1, list CheckPointList; the home stage's own, else
    those of the zones its ZoneList places), or at the default entrance when the scenario places none. A flag is named
    the way the game names a placement (al::PlacementId::makeString): its CommonId if it has one, else its Id, in a zone
    "<Id>(<zone>[<the zone's Id in the stage>])"."""
    import functools
    import sarc as S
    from byml import Byml, sarc_files

    @functools.lru_cache(maxsize=None)
    def map_of(stage):
        files = sarc_files(S.yaz0(open(T.build('romfs', 'StageData', stage + 'Map.szs'), 'rb').read()))
        return Byml(files[stage + 'Map.byml']).root()

    def entries(stage, n, name):
        scen = map_of(stage)
        return [o for o in (scen[n - 1].get(name) or []) if isinstance(o, dict)] if isinstance(scen, list) and n - 1 < len(scen) else []

    def flags_of(stage, n, zone=None):
        return [o.get('CommonId') or (o.get('Id') if not zone else '%s(%s[%s])' % (o.get('Id'), zone[0], zone[1]))
                for o in entries(stage, n, 'CheckPointList') if o.get('UnitConfigName') == 'CheckpointFlag']

    def mode_of(stage, n):
        """The mode an NPC starts in this scenario, which a save cannot hold: the Koopa race (RaceManRace placed) or
        Luigi's Balloon World (its NPC without the race's start, or its invalid areas)."""
        scen = map_of(stage)
        names = {o.get('UnitConfigName') for lst in (scen[n - 1].values() if n - 1 < len(scen) else [])
                 if isinstance(lst, list) for o in lst if isinstance(o, dict)}
        if 'RaceManRace' in names:
            return 'race'
        if 'BalloonGameSettingInvalidArea' in names or ('TimeBalloonNpc' in names and 'RaceManStart' not in names):
            return 'balloon'
        return None
    files = sarc_files(S.yaz0(open(T.build('romfs', 'SystemData', 'WorldList.szs'), 'rb').read()))
    count = {w.get('Name'): w.get('ScenarioNum') or 0 for w in Byml(files['WorldList.byml']).root()}
    out = []
    for world, w in enumerate(worlds):
        home = w.get('Name')
        for n in range(1, count.get(home, 0) + 1):
            zones = [(z.get('UnitConfigName'), z.get('Id')) for z in entries(home, n, 'ZoneList')
                     if os.path.exists(T.build('romfs', 'StageData', z.get('UnitConfigName', '') + 'Map.szs'))]
            flags = flags_of(home, n) or [f for z in zones for f in flags_of(z[0], n, z)]
            out.append({'id': '%s_s%d' % (home, n), 'stage': home, 'world': world, 'world_name': w.get('WorldName'),
                        'category': 'Scenario', 'scenario': n, 'checkpoint': flags[(n - 1) % len(flags)] if flags else None,
                        'mode': mode_of(home, n)})
    return out


def entry(stage):
    return {k: stage[k] for k in ('id', 'stage', 'category', 'scenario', 'checkpoint', 'mode') if stage.get(k) is not None} | {'world': stage['world_name']}


def run_totals(t):
    """The counters of the end-of-run summary at the end of this part of a run's log: its last "stage list: ... run
    totals" line, else the end-of-run summary itself (the run ended in this part); None when there is neither (a crash
    without a summary)."""
    m = list(TOTALS.finditer(t))
    if m:
        return dict(zip(TOTAL_KEYS, (float(x) if '.' in x else int(x) for x in m[-1].groups())))
    f = re.search(r'display: (\d+) frames presented', t)
    vk = re.search(r'nvn_vk: \d+ frames presented, .*?(\d+) Vulkan call failures, .*?(\d+) calls of operations that are not implemented', t)
    im = re.search(r'imports reached: (\d+)\s+\(implemented: (\d+), still stubs: (\d+)\)', t)
    au = re.search(r'device sink: (\d+) frames output, \d+ blocks of 40 ms queued, (\d+) samples dropped \(device behind\), (\d+) underruns', t)
    if not (f and vk and im and au):
        return None
    holds = re.findall(r'loading-screen frames held for the workers: (\d+), (\d+) ms in all', t)
    dummies = re.findall(r'samplers given a dummy because the texture is of another kind than the shader declares: (\d+)', t)
    return {'frame': None, 'frames': int(f.group(1)), 'vk_failures': int(vk.group(1)), 'not_implemented': int(vk.group(2)),
            'imports_reached': int(im.group(1)), 'imports_stub': int(im.group(3)), 'audio_frames': int(au.group(1)),
            'audio_dropped': int(au.group(2)), 'audio_underruns': int(au.group(3)),
            'load_holds': int(holds[-1][0]) if holds else 0, 'load_hold_ms': int(holds[-1][1]) if holds else 0,
            'dummies': int(dummies[-1]) if dummies else 0, 'memory_mb': None, 'run_s': None}


def scenario_from(said):
    """Whose scenario a stage started in, from the runtime's words: 'asked for' (SMO_START_STAGE=<stage>:<n>, a list
    entry's), "the stage's own" (the one the game's stage change gives it, as through its door), "the kingdom's"."""
    return None if not said else 'asked for' if said.startswith('asked') else "the stage's own" if said.startswith("the stage's own") \
        else "the kingdom's"


def read_log(path, stage, before=None, first=True, text=None):
    """The results of one stage from its run's log (or text, a part of it). A part of the log of a run of several stages
    (--chain) has its counters (Vulkan failures, operations not implemented, imports, audio, frames, ...) as the run's
    totals at its end less those at its start (before: the part before it; None for the first part), a stage after the
    first its load time from the moment its stage change was asked for, and restarts the stages the game started by
    itself (a miss)."""
    t = text if text is not None else open(path, encoding='utf-8', errors='replace').read() if os.path.exists(path) else ''
    r = entry(stage)
    m = re.search(r'StageSceneNrvPlay seen (?:after \d+ polls )?\(frame (\d+)\)', t)
    r['play_frame'] = int(m.group(1)) if m else None
    m = re.search(r'start stage: frame \d+: the game starts (\S+), scenario (-?\d+); SMO_START_STAGE makes it (\S+), scenario (-?\d+)(?: \(([^)]*)\))?', t)
    r['started'] = m.group(3) if m else stage['stage'] if 'SMO_START_STAGE' not in t else None   # the stage the scene was made for
    if m:                                                          # the scenario SMO_START_STAGE gave it, and whose it is
        r['scenario_started'], r['scenario_from'] = int(m.group(4)), scenario_from(m.group(5))
    resume = re.search(r'poll (\d+) step 1 keys Z', t)
    r['load_frames'] = r['play_frame'] - int(resume.group(1)) if r['play_frame'] and resume else None
    r['host_exceptions'] = t.count('HOST EXCEPTION')
    r['exception_lines'] = re.findall(r'HOST EXCEPTION[^\n]*', t)[:2]
    aborts = re.findall(ABORTS, t)
    r['aborts'], r['abort_lines'] = len(aborts), [x.strip()[:200] for x in aborts[:2]]
    r['glsl_rejected'] = t.count('glslang rejected the translated text')
    m = re.search(r'(\d+) Vulkan call failures', t)
    r['vk_failures'] = int(m.group(1)) if m else None
    m = re.search(r'(\d+) calls of operations that are not implemented', t)
    r['not_implemented'] = int(m.group(1)) if m else None
    r['not_implemented_kinds'] = sorted(set(re.findall(r'NOT IMPLEMENTED in the Vulkan back end, skipped[^:]*: ([^\n(]*)', t)))[:5]
    r['not_translated'] = sorted(set(re.findall(r'(program #\d+ \w+ stage: machine code not translated: [^\n]{0,120})', t)))[:5]
    m = re.search(r'imports reached: (\d+)\s+\(implemented: (\d+), still stubs: (\d+)\)', t)
    r['imports_reached'], r['imports_stub'] = (int(m.group(1)), int(m.group(3))) if m else (None, None)
    un = t.split('unimplemented imports reached:', 1)
    r['unimplemented_imports'] = [l.strip() for l in un[1].split('\n\n', 1)[0].strip().split('\n') if l.strip()][:5] if len(un) > 1 else []
    r['freezes'] = t.count('NO FRAME PRESENTED')
    r['dummy_textures'] = len(re.findall(r'a dummy is bound', t))
    r['uniform_handle_draws'] = re.findall(r'(program #\d+, \w+ stage: texture handle [^\n]*)', t)[:3]
    m = re.search(r'device sink: \d+ frames output, \d+ blocks of 40 ms queued, (\d+) samples dropped \(device behind\), (\d+) underruns', t)
    r['audio_dropped'], r['audio_underruns'] = (int(m.group(1)), int(m.group(2))) if m else (None, None)
    m = re.search(r'loading-screen frames held for the workers: (\d+), (\d+) ms in all, longest (\d+) ms', t)
    r['load_hold_ms'], r['load_hold_longest_ms'] = (int(m.group(2)), int(m.group(3))) if m else (None, None)
    heavy, worst, wins, medians = 0, 0.0, 0, []
    for m in re.finditer(r'nvn_perf: frames (\d+)\.\.\d+ frame time ms: avg [\d.]+ min [\d.]+ median ([\d.]+) p99 ([\d.]+) max ([\d.]+)', t):
        if r['play_frame'] and int(m.group(1)) >= r['play_frame'] + 300:
            wins += 1
            worst = max(worst, float(m.group(4)))
            heavy += float(m.group(3)) > 20.0
            medians.append(float(m.group(2)))
    # information only: at the raised speed, with jobs side by side, frame times are not those of normal play
    r['windows_after_play'], r['windows_p99_over_20ms'], r['worst_frame_ms'] = wins, heavy, round(worst, 1)
    r['median_frame_ms'] = round(sorted(medians)[len(medians) // 2], 2) if medians else None
    m = re.search(r'display: (\d+) frames presented', t)
    r['frames'] = int(m.group(1)) if m else None
    r['time_limit'] = 'still running after' in t                  # ended by the time limit, not with its input sequence
    r['sequence_done'] = 'input sequence: done at poll' in t
    if 'stage list: ' in t:                                        # a part of a run of several stages (--chain)
        m = re.search(r'stage list: frame \d+: stage \d+ of \d+ begins: the game starts (\S+), scenario (-?\d+)', t)
        r['started'], r['scenario_started'] = (m.group(1), int(m.group(2))) if m else (None, None)
        if not first:
            m = re.search(r'the game is asked to change stage at its next StageSceneNrvPlay \(frame (\d+)\)', t)
            r['load_frames'] = r['play_frame'] - int(m.group(1)) if r['play_frame'] and m else None
            m = re.search(r'tryChangeNextStage to \S+, (?:scenario -?\d+ \(([^)]*)\)|(the kingdom\'s) scenario)', t)
            r['scenario_from'] = scenario_from(m.group(1) or m.group(2)) if m else None
        end = run_totals(t)
        d = {k: end[k] - (before or {}).get(k, 0) for k in TOTAL_KEYS if k not in ('frame', 'memory_mb', 'run_s')} if end else None
        for k in ('frames', 'vk_failures', 'not_implemented', 'imports_reached', 'imports_stub', 'audio_dropped', 'audio_underruns', 'load_hold_ms'):
            r[k] = d[k] if d else None
        r['load_hold_longest_ms'] = None
        if d and d['dummies'] and not r['dummy_textures']:        # the dummy lines are printed for a run's first 20 only
            r['dummy_textures'] = min(20, d['dummies'])
        r['memory_mb'] = end['memory_mb'] if end else None         # the process's at the end of the stage (information)
    r['restarts'] = re.findall(r': frame \d+: the game starts (\S+), scenario -?\d+ itself', t)   # stage scenes the game started itself
    r['problems'] = problems(r)
    if 'stage list: ' in t and not first:                          # started by its stage change, not by SMO_START_STAGE
        r['problems'] = [p.replace('(SMO_START_STAGE)', '(its stage change, SMO_STAGE_LIST)') for p in r['problems']]
    return r


def is_demo(stage):
    return stage['category'] == 'Demo' or stage['stage'].startswith('Demo')


def not_a_stage_scene(stage):
    """Stages the game does not play as a stage scene of their own, so SMO_START_STAGE cannot start them the game's way:
    cutscenes (DemoScene, DemoChangeWorldScene, EndingScene and WorldWarpHoleScene start them, GameDataFunction::
    startDemoStage first), the Labo VR mode's *Rsv* stages (the VR mode's own start), the world map (a state inside a
    stage scene). Started as a stage scene anyway (--special) they stop in the game's own code."""
    return is_demo(stage) or 'Rsv' in stage['stage'] or stage['stage'] == 'WorldMapStage'


_maps = {}


def stage_map(T, stage):
    """The scenarios of a stage's map (StageData/<stage>Map.szs): a list of dictionaries of object lists; [] when the
    stage has no map."""
    if stage not in _maps:
        import sarc as S
        from byml import Byml, sarc_files
        p = T.build('romfs', 'StageData', stage + 'Map.szs')
        root = Byml(sarc_files(S.yaz0(open(p, 'rb').read()))[stage + 'Map.byml']).root() if os.path.exists(p) else []
        _maps[stage] = root if isinstance(root, list) else [root]
    return _maps[stage]


def painting_entry(T, stage):
    """How the game enters a stage that only a painting leads to (the Mushroom Kingdom's boss rematches): (the painting's
    room, the start point its stage change names), from the maps: the stage's MissRestartArea sends a miss back to the
    room, and the room's PictureStageChange to the stage names the start point. None for any other stage. Started on
    its own, such a stage meets a restart after a miss without the room's archives loaded, which the game never does."""
    def objs(name):
        return [o for scen in stage_map(T, name) for lst in scen.values() if isinstance(lst, list) for o in lst if isinstance(o, dict)]
    room = next((o.get('ChangeStageName') for o in objs(stage['stage']) if o.get('UnitConfigName') == 'MissRestartArea'), None)
    start = next((o.get('ChangeStageId') for o in objs(room) if o.get('UnitConfigName') == 'PictureStageChange'
                  and o.get('ChangeStageName') == stage['stage']), None) if room else None
    return (room, start) if start else None


def problems(r):
    p = []
    if r['host_exceptions']:
        p.append('host exception')
    if r.get('started') != r['stage']:
        p.append('the stage was not started (SMO_START_STAGE)')
    if r['aborts']:
        p.append('%d aborts or failed assertions' % r['aborts'])
    if r['glsl_rejected']:
        p.append('%d translated stages rejected by glslang' % r['glsl_rejected'])
    if r['play_frame'] is None and not is_demo(r):
        p.append('no play')
    elif r['time_limit'] and not is_demo(r):
        p.append('input sequence not finished in time')
    if r['vk_failures']:
        p.append('%d Vulkan failures' % r['vk_failures'])
    if r['not_implemented']:
        p.append('%d not implemented' % r['not_implemented'])
    if r['not_translated']:
        p.append('%d shaders not translated' % len(r['not_translated']))
    if r['imports_stub'] or r['unimplemented_imports']:
        p.append('unimplemented imports')
    if r['freezes']:
        p.append('%d freeze reports' % r['freezes'])
    if r['dummy_textures']:
        p.append('%d dummy textures' % r['dummy_textures'])
    if r['audio_dropped'] or r['audio_underruns']:
        p.append('audio %s dropped %s underruns' % (r['audio_dropped'], r['audio_underruns']))
    if r['frames'] is None:
        p.append('no summary (crashed or killed?)')
    return p


def thumbs(shot_dir, play_frame, out_dir, stage, frames=None):
    """Four small pictures after play began (or spread over the run when it never began); the full-size ones are deleted,
    unless frames = (first, last) picks a stage's part of a run of several (--chain): those are deleted with the run."""
    from PIL import Image
    shots = sorted((int(re.search(r'(\d+)', n).group(1)), n) for n in os.listdir(shot_dir) if n.endswith('.bmp')) if os.path.isdir(shot_dir) else []
    if frames:
        shots = [s for s in shots if frames[0] <= s[0] <= frames[1]]
    kept = []
    if shots:
        wanted = [play_frame + a for a in SHOTS_AFTER_PLAY] if play_frame else [shots[len(shots) * k // 4][0] for k in range(1, 4)] + [shots[-1][0]]
        for w in wanted:
            f, n = min(shots, key=lambda s: abs(s[0] - w))
            dst = os.path.join(out_dir, '%s_%05d.jpg' % (stage, f))
            if dst in kept:
                continue
            try:
                Image.open(os.path.join(shot_dir, n)).convert('RGB').resize((400, 225)).save(dst, quality=80)
                kept.append(dst)
            except OSError:
                pass
    if not frames:
        shutil.rmtree(shot_dir, ignore_errors=True)
    return [os.path.basename(k) for k in kept]


def chain_key(stage):
    """Stages that may share a run (--chain): one world, and one kind: stages in the save's scenario, scenarios started at
    a checkpoint flag (the world visited before), scenarios at their default start."""
    return stage['world'], stage['category'] == 'Scenario', bool(stage.get('checkpoint'))


def chains_of(stages, n):
    """A job's stages, in order, in runs of up to n stages of one chain_key each; a scene stage (no play to end its
    input sequence) and a stage entered from a painting's room run alone."""
    out = []
    for s in stages:
        alone = is_demo(s) or s.get('entry')
        if n > 1 and out and len(out[-1]) < n and not alone and not is_demo(out[-1][0]) and not out[-1][0].get('entry') \
                and chain_key(out[-1][0]) == chain_key(s):
            out[-1].append(s)
        else:
            out.append([s])
    return out


def split_run(text, n):
    """The log of a run of n stages (SMO_STAGE_LIST) in parts, one per stage that ran: each part ends with the line that
    ends its stage ("stage list: stage k of n done ..."), the first holds the boot. What follows the last such line goes
    to the last stage when every stage is done (the end-of-run summary), and to the next stage when it began or the run
    crashed while changing to it; else (the change never came: a time limit, a kill) it is the next stage's part all
    the same, but that stage did not run. Returns (parts, the number of parts that are a stage that ran)."""
    ends = [m.end() for m in STAGE_DONE.finditer(text)]
    parts = [text[a:b] for a, b in zip([0] + ends, ends)]
    rest = text[ends[-1]:] if ends else text
    if len(parts) == n:
        parts[-1] += rest
        return parts, n
    began = re.search(r'stage list: frame \d+: stage %d of %d begins' % (len(parts) + 1, n), rest)
    ran = not parts or began or 'HOST EXCEPTION' in rest or re.search(ABORTS, rest) or 'GUEST TRAP' in rest
    return parts + [rest], len(parts) + bool(ran)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--save', required=True, help='a save folder to start from (only read)')
    ap.add_argument('--target', default=os.environ.get('SMO_TARGET', '1.0.0'))
    ap.add_argument('--stages', default='all')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--speed', default='4')
    ap.add_argument('--seconds', type=int, default=150, help='time limit of a run; a run ends sooner, with its input sequence')
    ap.add_argument('--cache', help='shader cache folder each job starts from (copied)')
    ap.add_argument('--name', default=time.strftime('stress_%Y%m%d_%H%M'))
    ap.add_argument('--zones', action='store_true', help='also start in the zones (parts placed inside stages)')
    ap.add_argument('--default-world', type=int, default=0, help='world of a stage the world list does not name')
    ap.add_argument('--scenarios', action='store_true', help="also every scenario of every kingdom (its home stage)")
    ap.add_argument('--env', action='append', default=[], metavar='K=V', help='more environment for the game (e.g. SMO_ACTORPOS=300)')
    ap.add_argument('--special', action='store_true', help='also the stages that are not stage scenes (cutscenes, VR, world map)')
    ap.add_argument('--cpus', choices=('split', 'shared'), default='split',
                    help='split: each job\'s game puts the console\'s three cores on processors of its own (SMO_OS_HOST_CPUS); '
                         'shared: all games on the same three, as two games started by hand would be')
    ap.add_argument('--chain', type=int, default=1, metavar='N',
                    help='up to N stages of one kingdom (and kind) in one run of the game, each changed to by the game itself '
                         '(SMO_STAGE_LIST); 1 = a run per stage')
    ap.add_argument('--exe-prefix', default='smo_stress', help='the jobs\' exe copies are <prefix>_<job>.exe')
    ap.add_argument('--direct', action='store_true',
                    help='start a stage that only a painting leads to directly, not from the painting\'s room (for comparisons)')
    a = ap.parse_args()
    stages, worlds, T = stage_list(a.target, a.zones, a.default_world)
    if not a.special:
        left = [s['stage'] for s in stages if not_a_stage_scene(s)]
        stages = [s for s in stages if not not_a_stage_scene(s)]
        print('not started (not stage scenes; --special starts them anyway): %d: %s' % (len(left), ' '.join(left)))
    if a.scenarios:
        stages += scenario_list(T, worlds)
    if a.stages != 'all':
        names = open(a.stages[1:]).read().split() if a.stages.startswith('@') else a.stages.split(',')
        stages = [s for s in stages if s['id'] in names or (s['stage'] in names and s['category'] != 'Scenario')]
    out = T.build('stress', a.name)
    os.makedirs(os.path.join(out, 'thumbs'), exist_ok=True)
    done = set()
    res_path = os.path.join(out, 'results.jsonl')
    if os.path.exists(res_path):                         # resumable: runs already in the results are skipped
        done = {(lambda r: r.get('id', r['stage']))(json.loads(l)) for l in open(res_path, encoding='utf-8') if l.strip()}
    todo = [s for s in stages if s['id'] not in done]
    print('%d runs, %d to do, %d jobs, speed %s, at most %d s each -> %s' % (len(stages), len(todo), a.jobs, a.speed, a.seconds, out))
    for s in todo:                                      # stages only a painting leads to: entered from its room, as in play
        s['entry'] = None if a.direct else painting_entry(T, s)
        if s['entry']:
            print('%s: entered from %s through its painting (start %s)' % (s['id'], s['entry'][0], s['entry'][1]))
    sys.stdout.flush()
    caches = []
    for k in range(a.jobs):
        c = os.path.join(out, 'cache_%d' % k)
        if a.cache and not os.path.isdir(c):
            shutil.copytree(a.cache, c)
        caches.append(c)
    lock = threading.Lock()
    # each job takes whole worlds, its main stages first: a world's shaders are made once, in that job's cache
    order = {'MainStage': 0, 'MainRouteStage': 1}
    by_world = {}
    for s in todo:
        by_world.setdefault(s['world'], []).append(s)
    queues = [[] for _ in range(a.jobs)]
    for w in sorted(by_world, key=lambda w: -len(by_world[w])):
        min(queues, key=len).extend(sorted(by_world[w], key=lambda s: order.get(s['category'], 2)))

    homes = [w.get('Name') for w in worlds]
    sys.path.insert(0, os.path.join(REPO, 'tools', 'dev'))
    from save_warp import restart_of

    def warp_save(stage, work, reached=()):
        """A copy of the save that continues where the stage starts (a save resumes in a kingdom's home stage: any
        other stage is started from there by SMO_START_STAGE); None, or why it failed."""
        sub = stage['stage'] not in homes
        warp = [PY, os.path.join(REPO, 'tools', 'dev', 'save_warp.py'), a.save, '--out', os.path.join(work, 'save'),
                '--stage', homes[stage['world']] if sub else stage['stage'], '--world', str(stage['world'])]
        if stage.get('scenario'):
            warp += ['--scenario', str(stage['scenario'])]
        if stage.get('checkpoint'):                     # visited before: a first visit would start at the ship
            warp += ['--checkpoint', stage['checkpoint'], '--visited']
        for f in reached:
            warp += ['--reached', f]
        w = subprocess.run(warp, cwd=REPO, env=dict(os.environ, SMO_TARGET=a.target), capture_output=True, text=True)
        return 'warp failed: ' + (w.stdout + w.stderr).strip()[-200:] if w.returncode else None

    def game_cmd(name, k, stage, work, shots, seconds, timeout=None):
        cmd = [PY, os.path.join(REPO, 'scripts', 'run.py'), name, '--target', a.target, '--exe', '%s_%d.exe' % (a.exe_prefix, k),
               '--seconds', str(seconds), '--userdata', work,
               '--env', 'SMO_SEQ=' + sequence(stage), '--env', 'SMO_SEQ_EXIT=1', '--env', 'SMO_SPEED=' + a.speed,
               '--env', 'SMO_SHOT_EVERY=500', '--env', 'SMO_SHOT_DIR=' + shots]      # at most one capture a 300-frame window: p99 stays clean
        if timeout:
            cmd += ['--timeout', str(timeout)]
        if a.cache:
            cmd += ['--env', 'SMO_VK_CACHE=' + caches[k].replace('\\', '/')]
        if stage['stage'] not in homes:
            cmd += ['--env', 'SMO_START_STAGE=' + stage['stage']]
        ncpu = os.cpu_count() or 0
        if a.cpus == 'split' and a.jobs > 1 and ncpu >= 4 * a.jobs:     # each game its own processors for the console's three cores (os_threads.c)
            per = ncpu // a.jobs
            cmd += ['--env', 'SMO_OS_HOST_CPUS=%x' % (((1 << per) - 1) << (ncpu - per * (k + 1)))]
        for e in a.env:
            cmd += ['--env', e]
        return cmd

    def record(r):
        with lock:
            with open(res_path, 'a', encoding='utf-8') as f:
                f.write(json.dumps(r) + '\n')
            print('%-44s %-10s %s' % (r['id'], 'play %s' % r.get('play_frame'), '; '.join(r['problems']) or 'ok'))
            sys.stdout.flush()

    def run_one(stage, k):
        name = '%s_%s' % (a.name, stage['id'])
        work = os.path.join(out, 'work_%d' % k)
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        why = warp_save(stage, work)
        if why:
            r = entry(stage) | {'problems': [why]}
        else:
            shots = os.path.join(work, 'shots')
            os.makedirs(shots)
            t0 = time.time()
            subprocess.run(game_cmd(name, k, stage, work, shots, a.seconds), cwd=REPO, env=dict(os.environ, SMO_TARGET=a.target),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            r = read_log(T.build('run', name + '.log'), stage)
            r['seconds'] = round(time.time() - t0)
            r['thumbs'] = thumbs(shots, r['play_frame'], os.path.join(out, 'thumbs'), stage['id'])
        shutil.rmtree(work, ignore_errors=True)
        record(r)

    runs = iter(range(1, 1 << 30))

    def run_chain(chain, k):
        """One run of the game for the stages of a chain: the first starts as run_one starts it, each next one through
        the game's own stage change when the one before has played its input sequence (SMO_STAGE_LIST); the time
        limit is per stage. The log is split per stage (build/<version>/run/<name>_<stage>.log, as run_one's) and each
        stage is recorded as its own run would be. Returns the stages a crash or a time limit left unplayed."""
        with lock:
            name = '%s_run%04d' % (a.name, next(runs))
        work = os.path.join(out, 'work_%d' % k)
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        flags = [s['checkpoint'] for s in chain[1:] if s.get('checkpoint') and s['checkpoint'] != chain[0].get('checkpoint')]
        why = warp_save(chain[0], work, sorted(set(flags)))
        if why:
            record(entry(chain[0]) | {'problems': [why]})
            shutil.rmtree(work, ignore_errors=True)
            return chain[1:]
        items = []
        for s in list(chain):                           # <stage>[:<scenario>[:<start>]]; the first: the save's
            try:
                start = s['entry'][1] if s.get('entry') else \
                    restart_of(s['stage'], s['checkpoint']) if s.get('checkpoint') and s is not chain[0] else ''
            except SystemExit as e:                     # as save_warp would have failed for its own run
                record(entry(s) | {'problems': ['warp failed: %s' % e]})
                chain.remove(s)
                continue
            item = s['stage']
            if start:
                item += ':%s:%s' % (s.get('scenario') or '', start)
            elif s.get('scenario'):
                item += ':%d' % s['scenario']
            items.append(item)
        shots = os.path.join(work, 'shots')
        os.makedirs(shots)
        cmd = game_cmd(name, k, chain[0], work, shots, a.seconds, timeout=a.seconds * len(chain) + 300)
        cmd += ['--env', 'SMO_STAGE_LIST=' + ','.join(items)]
        for i, s in enumerate(chain[1:], 2):
            cmd += ['--env', 'SMO_SEQ_%d=%s' % (i, sequence(s, first=False))]
        t0 = time.time()
        subprocess.run(cmd, cwd=REPO, env=dict(os.environ, SMO_TARGET=a.target), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wall = time.time() - t0
        log = T.build('run', name + '.log')
        parts, ran = split_run(open(log, encoding='utf-8', errors='replace').read() if os.path.exists(log) else '', len(chain))
        if chain[0].get('lead_in'):                     # [a painting's room, the stage]: the stage's result, its whole log
            room, stage = chain
            path = T.build('run', '%s_%s.log' % (a.name, stage['id']))
            with open(path, 'w', encoding='utf-8', errors='replace') as f:
                f.write(''.join(parts))
            got = ran >= 2 and len(parts) >= 2
            end0 = run_totals(parts[0]) if got else None
            r = read_log(path, stage, end0, first=not got, text=parts[1] if got else parts[0])
            if not got:                                 # it never got there: what the run met on the way is its result
                r['play_frame'] = None
                r['problems'] = [p.replace('(SMO_START_STAGE)', '(through the painting in %s)' % room['stage']) for p in problems(r)]
            r['entered_from'] = '%s, start %s' % (room['stage'], stage['entry'][1])
            r['seconds'] = round(wall)
            lo = end0['frame'] if end0 and end0.get('frame') is not None else 0
            r['thumbs'] = thumbs(shots, r['play_frame'], os.path.join(out, 'thumbs'), stage['id'], frames=(lo, 1 << 40))
            r['run'] = name
            record(r)
            if os.path.exists(log):
                os.remove(log)
            shutil.rmtree(work, ignore_errors=True)
            return []
        before, lo = None, 0
        for i, (stage, part) in enumerate(zip(chain, parts)):
            path = T.build('run', '%s_%s.log' % (a.name, stage['id']))
            with open(path, 'w', encoding='utf-8', errors='replace') as f:
                f.write(part)
            if i >= ran:                                # the rest of a run that never changed to this stage
                continue
            r = read_log(path, stage, before, first=i == 0)
            end = run_totals(part)
            r['seconds'] = round(end['run_s'] - (before or {}).get('run_s', 0)) if end and end.get('run_s') is not None else \
                round(wall - (before or {}).get('run_s', 0))
            hi = end['frame'] if end and end.get('frame') is not None else 1 << 40
            r['thumbs'] = thumbs(shots, r['play_frame'], os.path.join(out, 'thumbs'), stage['id'], frames=(lo, hi))
            r['run'] = '%s, stage %d of %d' % (name, i + 1, len(chain))
            record(r)
            before, lo = end, hi
        if os.path.exists(log):
            os.remove(log)                              # every line of it is in the stages' logs
        shutil.rmtree(work, ignore_errors=True)
        return chain[ran:]

    def job(chains, k):
        while chains:
            chain = chains.pop(0)
            if chain[0].get('entry'):                   # the painting's room first, then the stage through its painting
                room = {'id': chain[0]['id'] + '_room', 'stage': chain[0]['entry'][0], 'world': chain[0]['world'],
                        'world_name': chain[0]['world_name'], 'category': 'PaintingRoom', 'lead_in': True}
                run_chain([room, chain[0]], k)
                continue
            rest = run_chain(chain, k) if len(chain) > 1 else run_one(chain[0], k)
            if rest:                                    # after a crash or a time limit: a new run for the stages left
                chains.insert(0, rest)

    queues = [chains_of(q, a.chain) for q in queues]
    if a.chain > 1:
        print('%d runs of the game for them (--chain %d)' % (sum(len(q) for q in queues), a.chain))
    threads = [threading.Thread(target=job, args=(q, k)) for k, q in enumerate(queues)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    rows = [json.loads(l) for l in open(res_path, encoding='utf-8') if l.strip()]
    with open(os.path.join(out, 'summary.md'), 'w', encoding='utf-8') as f:
        f.write('| stage | world | category | scenario, checkpoint | play | load frames | problems |\n|---|---|---|---|---|---|---|\n')
        for r in rows:
            f.write('| %s | %s | %s | %s | %s | %s | %s |\n' % (
                r['stage'], r.get('world'), r.get('category'), ', '.join(str(r[k]) for k in ('scenario', 'checkpoint', 'mode') if r.get(k)),
                r.get('play_frame'), r.get('load_frames'), '; '.join(r['problems']) or 'ok'))
    bad = [r for r in rows if r['problems']]
    print('%d runs, %d with something to look at -> %s' % (len(rows), len(bad), os.path.join(out, 'summary.md')))


if __name__ == '__main__':
    main()
