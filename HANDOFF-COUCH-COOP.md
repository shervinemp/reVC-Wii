# Handoff: couch co-op (Wii port)

Branch `couch-coop`. Hand this to whoever picks the branch up next — human or model —
along with `COUCH-COOP-DESIGN.md`, which is the design's source of truth and lives
beside this file.

## Where the branch stands

| | |
|---|---|
| base | rebased onto `definitive-qol` (shipping) at `580381b5` |
| head | `b8b2be40` |
| commits it adds | 13 |
| pre-rebase head | tagged locally `couch-coop-prerebase` → `d0796880` |
| **not built since the rebase** | it was rebased and pushed, then deliberately left unbuilt |

The rebase replayed all 13 commits with two conflicts, both resolved on the design's
terms — see "How the rebase was resolved" below. `Cam.cpp`, `Text.cpp` and `config.h`
auto-merged.

## The ethos: GTA:SA PS2 co-op, not split-screen

This is the whole design, and it is not negotiable without rewriting it:

- **Two players, one shared camera, one screen.** No split-screen. The model is the PS2
  *San Andreas* co-op the Wii shipped alongside.
- **Player 1 is the script driver.** `PlayerInFocus` stays `0`. Missions, the HUD,
  mission triggers, pickups and every `PlayerInFocus` assumption keep talking to
  player 1 exactly as they do today. That single fact is what makes the rest
  affordable — nothing has to learn that a second player exists.
- **Player 2 is a real ped** who walks, aims, shoots and rides along, and **never owns
  a mission**.
- **Freeroam only.** Co-op is **off while a mission runs**. This is a hard boundary,
  not a degradation: it removes the whole class of mission-script / cutscene-camera /
  respawn conflicts rather than managing them. It is also what SA MP actually did.
- **The shared camera is semi-top-down and fixed.** One camera for both, angled down,
  **fixed pitch and no yaw**, widening with separation so splitting up zooms out rather
  than losing someone off-screen. It **must not read `GetMouseX/Y` at all**.
- **The pointer is a reticle, not a steering input.** In co-op the reticle never rotates
  the camera, because the camera does not turn — so aiming has nothing to fight.
- **Per-player reticle colours**, so on one screen you can tell whose aim is whose.
- **Player 2 is not saved.** Spawns next to player 1 when co-op is switched on; the save
  format is untouched, so every existing save stays byte-identical and valid.

The two load-bearing decisions are the **fixed downward camera** and the **reticle-only
pointer**. Together they are why two players stop fighting over one view.

### The top open risk

**Player 2 dying in freeroam.** Respawn is driven off `PlayerInFocus`, which stays `0`,
so nothing respawns player 2 — a dead second ped is a corpse the camera then frames for
the rest of the session. In mission-free co-op this is *likely*, not an edge case. The
respawn symbol has not been located yet (three searches came back empty).

## The gates, and how far each got

- **Gate 0 — the shared camera.** Done on this branch. Adds a `MODE_WII_COOP` branch to
  the dispatch switch in `Cam.cpp` beside the (dead) `MODE_TOPDOWN`. Testable solo: with
  one ped it degenerates to a normal top-down follow.
- **Gate 1 — a second controller.** **Not done.** `WPAD_CHAN_0` is hardcoded in about
  nine places (`WiiPad.cpp`, `WiiPadState.cpp`, `WiiSpeaker.cpp`) and threading a
  channel through them is the *easy* half. **The hard half is that the port has exactly
  one input sink**: `WiiPadState.cpp` funnels every pad's pointer, cursor and reticle
  into one process-wide `lwjgl::Keyboard` and `lwjgl::Mouse`, which `Pad.cpp` and
  `Frontend.cpp` read directly. Buttons are already per-pad via `CapturePad(padID)`;
  the pointer is not. Two remotes today don't collide, they *overwrite* — both read
  channel 0, so one player drives two peds.
- **Gate 2 — a second ped.** **Not done.** `NUMPLAYERS` is **deliberately still 1**: a
  commit reverted the bump because raising it silently enables a live script path.
  Gate 2 means `1 → 2`, parameterising the two hardcoded `Players[0]` assignments in
  `Pools.cpp`, and a `CWorld::AddSecondPlayer()` that placement-news a `CPlayerPed` into
  the ped pool.

## What to do next, in order

1. **Build it.** `COUCH-COOP-DESIGN.md` and the branch are current with shipping, but the
   rebase was left unbuilt on purpose.
2. **Judge Gate 0 solo** — one controller, freeroam; the camera should just follow one
   ped. This is the point of doing it first.
3. **Gate 1's input sink** — the real work, and it is a change to how the engine receives
   input, not a `WiiPad.cpp` edit.
4. **Gate 2**, then the respawn risk above.

## Lay of the land

**The repo.** A Wii port of reVC (re3). Branches: `definitive-qol` is **shipping**,
`main` is the untouched default, `couch-coop` is this. `fork/*` and `origin/*` are other
people's remotes; `reVC-Wii` is ours.

**Toolchain.** devkitPPC gcc 15.2.0. **libogc must be ≥ 3.1.0.** Older libogc (3.0.4,
what devkitPPC r49.2 shipped) has a GX command-processor FIFO interrupt bug that freezes
the game at the frame present, with no CPU fault. Fix: `pacman -S libogc`. Read
`WII-LIBOGC-FREEZE.md`. Building with libogc < 3.1.0 brings the freeze back.

**Build.** In the devkitPro MSYS2 shell:

```
cd build-wii && ninja
```

Every `ninja` is a full ~250-file rebuild, because the version header regenerates each
run and embeds a build timestamp.

**Deploy.** `scripts/deploy-wii.bat E:` writes `boot.dol` + `meta.xml` to the SD card.
Always verify afterwards: **md5 == `build-wii/src/reVC.dol`** *and* the **banner (git
hash) == HEAD**. The dol embeds a build timestamp, so its md5 changes on every build —
the banner is the durable check. A deploy silently "succeeds" if the card is absent.

**Logging.** One switch, `CREATE_LOG` in `src/wii-port/WiiTrace.h`: `1` is shipping
(rare lines only — boot, saves, HOME, errors, the debounced stall line); `2` adds the
arena drain sample, the per-MEMID breakdown and per-asset lines, and is what made the
game hitch. **Keep logging near zero — signal, not trace.** `WiiTraceSetStep` is a
pointer store and is free; the watchdog reports the last step and the heap at a freeze.

**Debugging a hang.** The watchdog line names the last `WiiTraceSetStep` zone; the crash
handler writes `WII CRASH sig=…` to the log fd. If there's no such line, it wasn't a
signal-based fault.

## Traps that will bite you

- **CRLF noise.** `git status` shows ~900 files "modified" — that's line endings, not
  content. **Never `git add -A`; add single files.** `vendor/librw` is *vendored* (files
  tracked directly), not a submodule. For a merge or rebase, **`-X ignore-cr-at-eol`**
  makes whole-file phantom conflicts disappear — that is what `Cam.cpp` was.
- **The input singletons** are Gate 1's blocker: `WiiPadState.cpp` funnels every pad into
  one Keyboard/Mouse.
- **`WPAD_CHAN_0`** is hardcoded in ~9 places, plus `WiiSpeaker.cpp`.
- **The pause menu is shared.** Shipping added a **CHEATS** row and the branch adds
  `COUCH_COOP_TOGGLE`; both live between Options and Quit and both are kept.
- **Do not modify the user's original game assets** (`target256`, `.gxt`). This port
  never rewrites them; missing text goes through the compiled-in fallback table in
  `src/text/Text.cpp`.
- **The SD card is small.** Keep the debug log minimal.

## How the rebase was resolved

Worth keeping so the reasoning isn't relitigated:

- **`src/wii-port/WiiPad.cpp`** — shipping had grown the single-player pointer path
  (spin-up easing + the +45% "gain from intent" swing boost); the co-op commit added a
  `reticleOnly` flag. Resolution: **keep the single-player path whole and bypass it in
  co-op** — `if(applied <= 0.0f || reticleOnly) return false;`. That is the design's
  "reticle only" decision; `-X theirs` would have thrown the camera work away.
- **`src/core/MenuScreensCustom.cpp`** — both sides added a pause-menu row
  (CHEATS vs `COUCH_COOP_TOGGLE`). **Both kept.**

## Key files

| file | why |
|---|---|
| `COUCH-COOP-DESIGN.md` | the design; source of truth |
| `src/core/Cam.cpp` | `MODE_WII_COOP` — the shared camera (Gate 0) |
| `src/wii-port/WiiPad.cpp` | `steerCrosshair`, `reticleOnly` — the reticle-only decision |
| `src/core/MenuScreensCustom.cpp` | `COUCH_COOP_TOGGLE`, and the shared pause menu |
| `src/core/config.h` | `NUMPLAYERS` (still 1) |
| `src/core/Pools.cpp` | the two hardcoded `Players[0]` assignments (Gate 2) |
| `src/wii-port/WiiPadState.cpp` | the single Keyboard/Mouse sink (Gate 1) |
| `src/wii-port/WiiSpeaker.cpp` | the remote speaker, for whoever owns the reticle |
| `WII-LIBOGC-FREEZE.md` | why libogc ≥ 3.1.0 is required |
