# Scripted runs

A scripted run plays the game with no input but a sequence file, and checks what happened against the game's own
state. It is how gameplay is tested here: the same sequence, played after a change, must still reach the same
checkpoints. `tests/run_cap_kingdom.py` is the example: from an empty save to Mario's first capture in Top-Hat Tower.

```bat
.venv\Scripts\python scripts\run.py <run name> --seconds 600 --seq tests\cap_kingdom.seq --userdata build\run\<run name>_save --env SMO_SEQ_EXIT=1
```

## Sequences

A sequence (`SMO_SEQ`; `--seq <file>` reads it from a file, `#` starts a comment) is a list of steps, executed in order
and counted in pad polls (the game polls player 1 once per frame), so it does not depend on how long loading took:

| Step | Meaning |
|---|---|
| `<n>` | wait n polls |
| `<n>:<keys>[/<h>]` | wait n polls, then hold the keys for h polls (default 8) |
| `@nerve:<state>` | wait until the game runs that state, e.g. `@nerve:OpeningStageStartDemoNrvWaitStandUp` |
| `@until-nerve:<state>:<keys>/<p>` | press the keys every p polls until the state runs (e.g. advance a talk until play resumes) |
| `@goto:<x>,<z>[,<r>][/<n>]` | steer the left stick to the world point (x, z) until the player is within r (default 100), at most n polls |
| `@near:<state prefix>[,<r>][/<n>]` | steer to the nearest actor whose state starts with the prefix until within r (default 250); the player then faces it |
| `@at:<x>,<y>,<z>[,<r>]` | wait until the player is within r (default 300) of the point, e.g. where a new stage puts him |
| `@warp:<x>,<y>,<z>` | test only: move the player to the point (the game's own `al::updatePoseTrans`, as when another actor moves him), for a test whose way there is not scripted |
| `@chase:<state prefix>[,<r>]/<n>` | for n polls, follow that actor, standing still within r (for exploring) |
| `@draws:<lo>-<hi>[/<n>]` | wait until n frames in a row had between lo and hi draw calls (a screen recognised by its draw count) |
| `@until:<lo>-<hi>[/<n>]:<keys>/<p>` | press the keys every p polls until the draw count is in that range |
| `@movie` | wait until a movie is opened |

Keys: `Z` `X` `C` `V` = A B X Y, `Q` `E` = L R, `A` `S` = ZL ZR, `!` = Plus, `-` = Minus, `I` `J` `K` `L` = d-pad
up/left/down/right, `u` `d` `l` `r` = left stick, `8` `2` `4` `6` = right stick up/down/left/right, `*` = the amiibo
menu's key. Every step is announced in the log with its poll and frame number.

While the amiibo menu is open the game is held, but the sequence goes on, one poll per frame of the menu: for example
`@nerve:StageSceneNrvPlay 240 1:* 30 1:E/4 20 1:Z 900` opens it, moves a page down (to Peach), chooses it and lets the
game read it (the log's `amiibo menu:`, `nfp:` and `nn::hid:` lines say what happened).

## The game's state

Every state machine of the game is an `al::NerveKeeper`: its current state (a "nerve") runs once a frame through a call
the runtime sees (`runtime/game/smo_nerve.c`). A state's name is the game's own class name for it, for example
`StageSceneNrvPlay` or `OpeningStageStartDemoNrvCameraTutorial`. Names come from the function list
(`build\functions\main_functions.tsv`): the class names the game's RTTI gives. The tracker names the others from the
code: a state whose
code calls `<Class>::exe<State>()` is `<Class>Nrv<State>`, one that runs a sub-state is named after its neighbours,
`<Class>Nrv_<address>`. Many classes are called `HostTypeNrv<State>` by the game itself (one per actor class); such a
state gets the class of the object that first runs it: `FrogNrvWait`.

- `SMO_NERVELOG=*` (or a list of substrings) logs every change of state with its frame number: the first thing to run
  when a scripted run gets stuck.
- `SMO_ACTORPOS=<n>[:<prefixes>]` logs where actors are (the player by default); following another actor's path, such as
  Cappy running ahead in the opening, gives waypoints that are known to be walkable.
- `SMO_ICALLS=<start>,<seconds>` counts which functions run in a window; the state that runs every frame stands out.
- The tracker costs time on every indirect call. It is on when a sequence has a step that needs it (`@nerve`,
  `@until-nerve`, `@goto`, `@near`, `@at`, `@chase`) and stops by itself once no such step is left, unless
  `SMO_NERVELOG` or `SMO_ACTORPOS` asks for it; so a timing run that only waits for a state at the start runs at full
  speed after it (the log says `nerve: tracking stops at frame F`).

## Steering

`@goto` turns a world direction into a stick direction the way the game does: it reads the game's camera (position and
look-at point), takes its forward direction on the ground plane as stick up and its right as stick right, and corrects
every poll, so the camera may turn while Mario runs. When the player moves less than 30 units in 12 polls, the step
holds A for 14 polls (a jump, enough for a step up), then waits 10 polls before it may jump again. A step ends early
when a cutscene takes the player for 10 polls, and when a new stage starts (a new player object).

## Finding a route

- The stage's placement data has the paths the game's own route guide shows: `tools\dev\byml.py
  StageData\<Stage>Map.szs --scenario 0 --grep RouteGuide` lists the objects; their rail points (in the BYML) are good
  waypoints.
- Doors, switches and moving parts often need something before they let Mario through. Their code says what: in Cap
  Kingdom the way down to Top-Hat Tower rises only after a cap hits the `CapSwitchSave`, and the tower door opens only
  on a cap hit (`DoorAreaChange::receiveMsg`); a `V` (Y, throw) facing the object does it.
- A copy of an earlier run's save folder (`--userdata`) starts from the last autosave, which makes trying the next part
  of a route quicker than starting from a new game every time.

## The Cap Kingdom run

`tests\run_cap_kingdom.py <new run name>` plays `tests\cap_kingdom.seq` from an empty save and checks 14 checkpoints:

1. the mode select of a new game
2. the prologue skipped through its own menu (Plus twice, then Skip)
3. the opening's camera tutorial passed: it ends when the camera looks more than 60 degrees away from straight down,
   so the right stick tilts it
4. Mario stands up
5. gameplay begins
6. Cappy reached on the bridge after his three runs
7. Cappy joins: the cap-throw tutorial
8. the cap thrown
9. Bonneton's stairs climbed
10. the cap switch hit and the path risen
11. the tower door opened by the cap
12. inside Top-Hat Tower (a new stage)
13. a frog captured
14. the capture's message ("You captured a frog!") closed with A

It also requires 0 host exceptions, 0 Vulkan call failures, 0 operations not implemented, 0 unknown command handles,
and no dropped audio samples or underruns. The run ends with its sequence (`SMO_SEQ_EXIT=1`), a second after play
goes on as the frog: about 5 minutes with a warm shader cache. The checkpoints land on the same frames within a few
frames from run to run.

Limits: the route depends on the game behaving the same way each time (it does, as far as measured); the sequence
does not avoid enemies (a small Goomba hits Mario once on the way to the tower door).
