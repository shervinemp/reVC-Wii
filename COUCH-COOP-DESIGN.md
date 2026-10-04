# Couch co-op (GTA:SA PS2 model) — design

Branch: `couch-coop`, cut from `definitive-qol` at `1bd67275`.

Target model is the **PS2 GTA: San Andreas co-op** the Wii was built alongside:
two players, **one shared camera, one screen**, no split-screen. Player 1 stays
the script driver; player 2 is a real second ped who walks, aims, shoots and rides
along, but never owns a mission.

## The decisions

1. **Semi-top-down shared camera.** One camera for both players, angled down.
2. **The pointer is a reticle only.** In co-op it never rotates the camera — the
   camera does not turn, so aiming has nothing to fight.
3. **Per-player reticle colours**, so on a shared screen you can tell whose aim is
   whose at a glance.
4. **`PlayerInFocus` stays 0.** All scripts, the HUD, mission triggers and pickups
   keep talking to player 1 exactly as they do now. This is what makes the whole
   thing affordable.
5. **Player 2 spawns next to player 1** when co-op is switched on, and is **not
   saved** — see "Save format" below.

Decisions 1 and 2 are the load-bearing ones, and they are the reason this design
works. The single-player scheme is pointer-*driven* aiming: the crosshair is the
IR dot and the camera chases it (`irAimRate`). With a fixed downward camera there
is no chase, so the reticle decouples from the view for free and two players stop
fighting over one camera.

## Verified anchors

Facts below were read out of this tree, not assumed.

| Thing | Where |
|---|---|
| Player slots | `src/core/config.h:16` — `NUMPLAYERS = 1`, doubling as the `CWorld::Players[]` array size (`World.h:62`) |
| Player ped is born hardcoded to slot 0 | `src/core/Pools.cpp:589` and `:637`, both `CWorld::Players[0].m_pPed = …` |
| Single focus player, read by everything | `#define PLAYER (CWorld::Players[CWorld::PlayerInFocus].m_pPed)` — `src/core/Camera.cpp:71` |
| Save identifies the player ped by type | `CPools::SavePedPool` / `LoadPedPool`, `src/core/Pools.cpp:504`/`:552`, gated on `m_nPedType == PEDTYPE_PLAYER1` |
| **Wiimote channel is hardcoded** | `WPAD_CHAN_0` at `WiiPad.cpp:982,986,1005,1011,1046,1083,1195,1344` and `WiiPadState.cpp:485,502`, plus `WiiSpeaker.cpp:33` |
| GameCube pads *are* per-pad | `CapturePad(padID)` → `WiiPadCapture(padID, …)` → `captureGameCube(padID, …)` — `wii_game.cpp:445`, `WiiPad.cpp:1154` |
| Camera target is a single entity | `Cam.cpp:163` — `CameraTarget = CamTargetEntity->GetPosition()` |
| Top-down mode exists but is dead | `Camera.h:38` declares `MODE_TOPDOWN`; `Cam.cpp:176` has its handler **commented out** |
| Peds are placement-new'd into a fixed pool | `typedef CPool<CPed,CPlayerPed> CPedPool` — `Pools.h:16`; `CPool` has no `Allocate` (`templates.h:38`) |

## The three gates, in dependency order

### Gate 0 — the shared camera (do this first)

The engine's top-down camera is dead code, so this is written from scratch. Add a
`MODE_WII_COOP` branch to the dispatch switch in `Cam.cpp` (beside `MODE_TOPDOWN`
at :174) that:

- computes a target as the **midpoint of the live player peds**, clamped so the
  pair cannot drag the view somewhere neither of them is;
- places the camera at a **fixed offset above and behind that midpoint** — fixed
  pitch, no yaw, which is precisely what stops aiming from rotating the view;
- widens with separation, GTA:SA-style, so splitting up zooms out rather than
  losing someone off the edge;
- ignores mouse/pointer input entirely. It must not read `GetMouseX/Y` at all.

With one player it degenerates to a normal top-down follow, so it is testable
alone — that is the point of doing it first.

### Gate 1 — a second controller

`WPAD_CHAN_0` is hardcoded in nine places. Everything else in the capture path is
already parameterised by `padID`, so the work is threading a channel through
instead of a constant. Two Wiimotes on channels 0 and 1, each with its Nunchuk;
player 1 keeps the pointer (channel 0), player 2 does not need one because under
this design aiming is a reticle and the camera is fixed.

The speaker (`WiiSpeaker.cpp:33`) is the fiddly one — it belongs to whoever's
remote owns the reticle.

Two Wiimotes currently produce something worse than unsupported: **both pads read
channel 0**, so one player would drive two peds. Worth fixing regardless of co-op.

### Gate 2 — a second ped

- `NUMPLAYERS` 1 → 2.
- Parameterise the two hardcoded `Players[0]` assignments in `Pools.cpp`.
- New `CWorld::AddSecondPlayer()`: placement-new a `CPlayerPed` into the ped pool
  (there is no `Allocate`; `CPool` slots are constructed in place), place it next
  to player 1, register it in `Players[1].m_pPed`.
- Because `PlayerInFocus` never leaves 0, nothing else has to learn that two
  players exist. That is the entire trick.

## Save format — player 2 is deliberately not persisted

`SavePedPool`/`LoadPedPool` gate on `PEDTYPE_PLAYER1` and `LoadPedPool` assigns
every match to `CWorld::Players[0]`. Two options:

- **Chosen:** player 2 is **not** `PEDTYPE_PLAYER1`, is not written to the save,
  and is re-spawned next to player 1 whenever co-op is switched on. Keeps every
  existing save byte-identical and valid — important, because save compatibility
  is already load-bearing on this port.
- Rejected: writing both peds means touching the save format and every load path,
  and buys nothing a player would miss.

## What this deliberately does not do

Stock missions stay single-player. Mission triggers, cutscene cameras and
`PlayerInFocus` all belong to player 1, so during a mission player 2 is a
bodyguard. That is exactly the PS2 SA co-op compromise, and the existing PC mods
hit the same wall — one of them ships "crashes if a cutscene shows up" as a known
issue and another simply blocks save/load from the menu.

Per-player *state* that a fuller co-op would want, from the feature list of the
existing PC mods and not yet in scope here:

- **wanted level synchronisation** — the stars cannot be player 1's problem alone
- **pickup ownership for player 2** — health, armour, weapons, money

Both are the same class of problem as mission ownership: per-player state the
engine keys off `PlayerInFocus`.

## Honest cost, and the testing problem

| | |
|---|---|
| Gate 0 — shared camera | ~2–3 days, **testable solo** |
| Gate 1 — channel plumbing | ~1 day, mechanical |
| Gate 2 — second ped | ~2–3 days |
| Co-op mission work | ongoing, per mission |
| Freeroam-only playable | ~1–2 weeks |

The testing problem is worse than the coding. This needs two humans, two
remotes, and eyes on a television. Dolphin will not take a second controller
reliably. Gate 0 is the only slice that can be verified alone, which is why it
goes first.