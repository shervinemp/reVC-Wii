# Handoff: couch co-op (Wii port)

Branch `couch-coop`. Hand this to whoever picks the branch up next — human or model —
along with `COUCH-COOP-DESIGN.md`, which describes the design as built and lives
beside this file.

## Where the branch stands

| | |
|---|---|
| base | rebased onto `definitive-qol` (shipping) at `801aaa35` |
| state | all three gates committed: the shared camera rebuilt, a second controller, a second ped |
| builds | yes — `build-wii/src/reVC.dol`, clean, libogc 3.1.0 |
| **run on hardware** | **no. Not once.** Everything below "builds" is unverified. |

The work was done without a console. It compiles, links, and every path was
written against the engine code it calls — but co-op needs two people, two
controllers and a television, and Dolphin does not boot this port. Treat the
first boot as the first test of all of it.

It has had one thing in place of a test: three independent read-throughs
against the engine source (camera and HUD; the session code; the ped, weapon
and pad hooks), each told to find faults rather than confirm. Between them they
found about two dozen, including a crash (a partner left on a bike with no
rider) and three ways for both players to end up stuck. All of the confirmed
ones are fixed in the tree; `COUCH-COOP-DESIGN.md` says what each fix is where
the behaviour it changed is described.

## What changed since the last handoff

The last handoff said Gate 0 was done and Gates 1 and 2 were not. That was not
the real state:

- **Gate 0 had never run and could not have worked** — the camera fought the
  engine's mode selection every frame, and its heading rule made the player spin.
  It has been rebuilt as a mode `CCamera::CamControl` requests.
- **Gate 1's "hard half" did not exist.** `WiiPadState.cpp` and its `lwjgl`
  Keyboard/Mouse singletons are a leftover from another port and are not
  compiled. The real work was a pad slot, a per-player reticle and a few pad-0
  reads in ped code.
- **Gate 2 is in**: `NUMPLAYERS` is 2, and `CCoop` (`src/core/Coop.cpp`) owns the
  second player's whole life.
- **The respawn risk is closed.** The symbol the last handoff could not find is
  `CGameLogic::Update`, and it only knows the player in focus; the partner's
  wasted/busted is `CCoop`'s own (3.5 s, then back beside player 1).

`COUCH-COOP-DESIGN.md` has the detail and a "What the first draft got wrong"
section. Read it before trusting any older note about this branch, including the
commit messages.

## The ethos, unchanged

- **Two players, one shared camera, one screen.** No split-screen.
- **Player 1 is the script driver.** `PlayerInFocus` stays `0`.
- **Player 2 is a real ped** who walks, aims, shoots and rides along, and **never
  owns a mission**.
- **Freeroam only.** Co-op is off while a mission runs.
- **Aiming never moves the camera.** Each pointer is a reticle.
- **Player 2 is not saved.** Existing saves stay byte-identical.

## How to test it, in order

Deploy, then read `sd:/apps/reVC/debug.log` afterwards — every state change
writes one `WII coop:` line.

**Alone first (one controller).** This is the old "judge Gate 0 solo" step, and
it now exercises the camera, the reticle aiming and the facing rule.

1. Pause → COUCH CO-OP → ON → resume, in freeroam. Expect `session on`, and the
   view cutting to above-and-behind, facing the way you were looking. **It must
   hold still.** If it pulses between two cameras, the mode request in
   `CamControl` is not sticking.
2. Walk unarmed: stick up is up the screen; Tommy faces where he walks.
3. Draw a gun: he turns to face the pointer and strafes; hold A with the stick to
   sprint and he faces where he runs again. Put the gun away: with fists he
   faces where he walks, and a punch lands on whoever is in front of him — the
   reticle plays no part in melee.
4. Fire: shots go from the gun to the reticle. The reticle is cyan, and red on a
   target. Swing the reticle right round behind him while holding fire: the
   shots should sweep round with the gun, not come out of his back. Then the
   **sniper rifle and the rocket launcher**: one shot per press, toward the
   reticle, no scope and no animation. (If Tommy throws a punch instead, the
   direct-fire branch at the top of `CPlayerPed::ProcessPlayerWeapon` is not
   being reached.)
5. Press − : four framings. Pick one by eye; it is the thing this camera most
   needs a human for. The choice is kept in the INI. While you are looking:
   everyone on screen should have a **shadow**, and nothing in the **bottom
   corners** of the picture should blink in and out as you walk (both needed
   fixes outside the camera code — `Shadows.cpp`, `Renderer.cpp`). Then walk
   along the foot of a tall building on the camera's side of the street: the
   view will come in tight. That is the known limit the fourth, overhead
   framing is for — check that it clears it.
6. Get in a car and drive: the view should follow the car's nose.
7. Start any mission: expect `session off: mission` and the ordinary camera.
   It should come back when the mission ends.

**Then with a second controller** (Wii Remote + Nunchuk on channel 2, or a
GameCube pad in port 2).

8. Connect it. The help box should say "Player 2: press any button to join";
   nothing else happens until one is pressed. Then expect `player 2 in` and a
   second Tommy beside the first, each with a coloured pip and health bar, the
   partner carrying a copy of player 1's weapons. (Playing player 1 on a
   GameCube pad with the launch remote still on is the case this is for: no
   idle second Tommy.)
9. Each stick and pointer moves only its own player. On a pad with no pointer,
   flick the right stick at something and then hold fire with the same thumb:
   the player should keep facing that way and strafe for as long as the trigger
   is held.
10. Player 1 into a car; partner presses 2 nearby → gets in as passenger. 2 again
    → out (stopped), or rolls out (moving). With an SMG, player 1's drive-by
    should still work: D-pad left/right picks the side. Then a **bike**: the
    partner should only be able to get on once player 1 is sitting on it, and
    appears on the back rather than climbing on. Get player 1 off and on again
    with the partner aboard — this is where a crash was found on paper.
11. Walk the two apart: each should slow from about 18 m and stop at 24 m, both
    still on screen, with a one-time notice in the help box. Then have player 1
    drive away from the partner: at 28 m the partner should appear in the
    passenger seat (`player 2 out (too far from player 1)` then `player 2 in
    (riding with player 1)`).
12. Get the partner killed → `wasted`, and back after a few seconds. The players
    can hurt each other; doing so should not raise the wanted level. Have the
    partner spend some ammo, then drive off and let the leash bring them back:
    they should return with what they had left, not a refill.
13. **Save at a safe house with the partner in the world, quit, reload.** One
    player should load. This is the test of the save staying untouched.
14. Unplug the partner's Nunchuk or switch the remote off → gone after ~6 s.

### Where I would look first when something is off

Honest ranking of what is most likely to be wrong, since none of it has run:

1. **Strafing and facing** (`CCoop::FacesAim`, `CPed::CalculateNewVelocity`,
   `WorkOutHeadingForMovingFirstPerson`). The maths is the engine's own with the
   stick read against the screen, but animation groups are easy to get subtly
   wrong and impossible to see without running.
2. **Getting into a car as a passenger.** The enter-car code has `IsPlayer()`
   special cases everywhere and was only ever exercised with a player as driver.
3. **The camera indoors and under bridges** (`Process_WiiCoop`'s line-of-sight
   clip). It comes in under the ceiling; whether that is usable is a judgement.
4. **Reticle to world** (`UpdateAim` in `Coop.cpp`). If shots land consistently
   to one side of the reticle, start at the chest-height plane.
5. **The first spawn** (`SpawnPartner`). A crash on `player 2 in` would be here
   or in the first `CPlayerPed::ProcessControl` for a ped that is not in focus.

## Lay of the land

**The repo.** A Wii port of reVC (re3). Branches: `definitive-qol` is **shipping**,
`main` is the untouched default, `couch-coop` is this. `fork/*` and `origin/*` are other
people's remotes; `reVC-Wii` is ours.

**Toolchain.** devkitPPC gcc 15.2.0. **libogc must be ≥ 3.1.0.** Older libogc (3.0.4,
what devkitPPC r49.2 shipped) has a GX command-processor FIFO interrupt bug that freezes
the game at the frame present, with no CPU fault. Fix: `pacman -S libogc`. Read
`WII-LIBOGC-FREEZE.md`.

**Build.** In the devkitPro MSYS2 shell:

```
cmake --build build-wii --target reVC -j8
```

Incremental builds work. A new source file needs `cmake build-wii` first — the
sources are globbed at configure time.

**Deploy.** `scripts/deploy-wii.bat E:` writes `boot.dol` + `meta.xml` to the USB stick.
Always verify afterwards: **md5 == `build-wii/src/reVC.dol`** *and* the **banner (git
hash) == HEAD**. A build of an uncommitted tree reports its hash with `-dirty`. A
deploy silently "succeeds" if the card is absent.

**Logging.** One switch, `CREATE_LOG` in `src/wii-port/WiiTrace.h`: `1` is shipping.
Co-op logs transitions only, plus twelve camera lines (one every five seconds) each
time the shared camera takes over. **Keep logging near zero — signal, not trace.**
The log, saves and INI are on the **SD card** (`sd:/apps/reVC/`); the USB stick is
NTFS and read-only on the Wii.

## Traps that will bite you

- **Line endings.** The tree is stored LF throughout: `definitive-qol` normalised it
  (`4a7ae058`, with `* text=auto` in `.gitattributes`), and the rebase onto
  `801aaa35` took the old whole-file CRLF churn out of this branch's own commits,
  which is why each of them is a small diff now. What a checkout writes to disk
  still depends on the machine's `core.autocrlf` / `core.eol`, so a script that
  assumes LF can rewrite a whole file. Edit in place, check `git diff --stat`
  before committing, and **never `git add -A`.**
- **Pad 1 is the engine's debug pad.** Do not route a controller into it. The
  partner is on `PAD_COOP`.
- **`FREE_CAM` is defined.** Code in the `#else` of `#ifdef FREE_CAM` is dead.
- **`CCamera::bWiiCoopCamera` is the menu toggle, not "co-op is active".** Read
  `CCoop::IsRunning()`.
- **`FindPlayerPed()` is player 1.** Code that means "a human-controlled ped"
  wants `IsAnyPlayerPed()`; code that reads a pad for a ped wants
  `GetPadFromPlayer()`.
- **Do not modify the user's original game assets** (`target256`, `.gxt`). Missing
  text goes through the fallback table in `src/text/Text.cpp`.
- **The SD card is small.** Keep the debug log minimal.

## Key files

| file | why |
|---|---|
| `COUCH-COOP-DESIGN.md` | the design as built; what is not done |
| `src/core/Coop.cpp`, `Coop.h` | `CCoop`: the session, the partner, each player's aim |
| `src/core/Cam.cpp` | `Process_WiiCoop` and the framings |
| `src/core/Camera.cpp` | `CamControl` requests the mode; `CoopCameraReplaces` |
| `src/wii-port/WiiPad.cpp` | which controller is which player; both pointers |
| `src/core/Pad.h` | `PAD_COOP`, and why not pad 1 |
| `src/peds/PlayerPed.cpp` | `GetPadFromPlayer`; the facing dispatch in `ProcessControl` |
| `src/peds/Ped.cpp` | strafing (`CalculateNewVelocity`), the jump, `SetDead` |
| `src/weapons/Weapon.cpp` | shots from the gun to the reticle |
| `src/renderer/Hud.cpp` | `DrawCoopMarks`: reticles, pips, health bars |
| `src/core/config.h` | `NUMPLAYERS` (2), and what raising it cost |
| `WII-LIBOGC-FREEZE.md` | why libogc ≥ 3.1.0 is required |
