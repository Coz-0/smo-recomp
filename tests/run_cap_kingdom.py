"""The scripted Cap Kingdom run: a new game from an empty save to Mario's first capture, checked against the game's state.

    python tests/run_cap_kingdom.py <run name> [--seconds 720] [--cache <stage cache dir>] [--shots N] [--check-only]

Plays tests/cap_kingdom.seq through scripts/run.py with a new, empty save folder (build/run/<run name>_save; the name must
be new), then reads build/run/<run name>.log and prints one line per checkpoint with the frame it was reached at, the
run's health counters (host exceptions, Vulkan call failures, operations not implemented, unknown command handles, audio
drops and underruns) and the frame rate. Exit code 0 only if every checkpoint was reached and every counter is zero.
Checkpoints are changes of the game's own state machines (SMO_NERVELOG, runtime/game/smo_nerve.c) and sequence steps, not
pictures; --shots N also saves a frame every N frames to build/run/<run name>_shots for looking at.
About 12 minutes with a warm stage cache (--cache, or the default build/cache/vk after a first run); a cold cache needs
more time for compiling shaders, so give it a larger --seconds.
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402
RUN = T.build('run')

# (what, regular expression on the log). Nerve lines read "nerve: frame F keeper K: A -> B".
CHECKPOINTS = [
    ('mode select shown (new game)', r'input sequence: .* nerve:SceneStateSelectModeNrvWait seen'),
    ('prologue skipped through its menu', r'-> DemoSceneStateSkipDemoNrvSkip$'),
    ('camera tutorial passed (camera tilted)', r'OpeningStageStartDemoNrvCameraTutorial -> OpeningStageStartDemoNrvWaitShowStandUpGuide'),
    ('Mario stands up', r'-> OpeningStageStartDemoNrvStandUp$'),
    ('gameplay begins', r'StageSceneNrvStartStageBgm -> StageSceneNrvPlay'),
    ('Cappy reached on the bridge (talk)', r'-> OpeningStageStartCapManHeroNrvEventTalkDemo$'),
    ('Cappy joins: cap-throw tutorial', r'-> InformationWindowCapMsgRequesterNrvAppearCapMsgCapThrow$'),
    ('cap thrown', r'HackCapNrvThrowStart -> HackCapNrvThrow$'),
    ('Bonneton stairs climbed', r'input sequence: .* goto -3889 4879 reached'),
    ('cap switch hit, path risen', r'-> CapSwitchNrvOnWait$'),
    ('tower door opened by the cap', r'-> DoorAreaChangeNrvOpenWait$'),
    ('inside Top-Hat Tower (new stage)', r'input sequence: .* player at .* within 800 of -500 0 13950'),
    ('frog captured', r'-> PlayerActorHakoniwaNrvHack$'),
]
NERVELOG = ('SceneStateSelectMode,DemoScene,OpeningStageStartDemo,StageSceneNrv,OpeningStageStartCapManHero,'
            'InformationWindowCapMsgRequester,HackCapNrv,CapSwitchNrv,DoorAreaChange,PlayerActorHakoniwaNrv')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--seconds', type=int, default=720)
    ap.add_argument('--cache', help='SMO_VK_CACHE: a stage cache folder from an earlier run')
    ap.add_argument('--shots', type=int, default=0)
    ap.add_argument('--check-only', action='store_true', help='only read an existing log')
    a = ap.parse_args()
    log = os.path.join(RUN, a.name + '.log')
    if not a.check_only:
        save = os.path.join(RUN, a.name + '_save')
        if os.path.exists(save):
            raise SystemExit('%s exists: the run must start from an empty save, choose another run name' % save)
        cmd = [sys.executable, os.path.join(REPO, 'scripts', 'run.py'), a.name, '--seconds', str(a.seconds),
               '--seq', os.path.join(REPO, 'tests', 'cap_kingdom.seq'), '--userdata', save,
               '--env', 'SMO_NERVELOG=' + NERVELOG, '--env', 'SMO_ACTORPOS=60']
        if a.cache:
            cmd += ['--env', 'SMO_VK_CACHE=' + a.cache]
        if a.shots:
            shots = os.path.join(RUN, a.name + '_shots')
            os.makedirs(shots, exist_ok=True)
            cmd += ['--env', 'SMO_SHOT_EVERY=%d' % a.shots, '--env', 'SMO_SHOT_DIR=' + shots]
        subprocess.run(cmd, cwd=REPO)
    text = open(log, encoding='utf-8', errors='replace').read()
    lines = text.splitlines()
    ok = True
    print('\nCap Kingdom run %s:' % a.name)
    for what, rx in CHECKPOINTS:
        r = re.compile(rx)
        hit = next((ln for ln in lines if r.search(ln)), None)
        m = re.search(r'frame (\d+)', hit) if hit else None
        print('  %-4s %-42s %s' % ('ok' if hit else 'MISS', what, ('frame ' + m.group(1)) if m else ''))
        ok &= hit is not None
    gave_up = sum(1 for ln in lines if 'GAVE UP' in ln and ln.startswith('input sequence'))
    exc = sum(1 for ln in lines if ln.startswith('HOST EXCEPTION'))
    vk = re.search(r'nvn_vk: (\d+) frames presented, .*?(\d+) Vulkan call failures, .*?(\d+) calls of operations that are not implemented', text)
    stale = re.search(r'command lists \((\d+) unknown handles\)', text)
    au = re.search(r'audio: device sink: (\d+) frames output, .*?(\d+) samples dropped .*?(\d+) underruns', text)
    fps = [float(x) for x in re.findall(r'nvn_perf: frames \d+\.\.\d+ .*fps avg ([\d.]+)', text)]
    print('  host exceptions %d; steering steps that gave up %d' % (exc, gave_up))
    if vk:
        print('  frames %s; Vulkan call failures %s; operations not implemented %s' % vk.groups())
        ok &= vk.group(2) == '0' and vk.group(3) == '0'
    else:
        print('  MISS no Vulkan summary (the run did not end normally)')
        ok = False
    if stale:
        print('  unknown command handles %s' % stale.group(1))
        ok &= stale.group(1) == '0'
    if au:
        print('  audio: %s frames, %s samples dropped, %s underruns' % au.groups())
        ok &= au.group(2) == '0' and au.group(3) == '0'
    if fps:
        print('  fps per 300 frames: mean %.1f, lowest %.1f' % (sum(fps) / len(fps), min(fps)))
    ok &= exc == 0
    print('  RESULT: %s' % ('PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
