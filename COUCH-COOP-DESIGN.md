# Couch co-op (GTA:SA PS2 model) — design

Branch: `couch-coop`, cut from `definitive-qol` at `1bd67275`.

Target model is the **PS2 GTA: San Andreas co-op** the Wii was built alongside:
two players, **one shared camera, one screen**, no split-screen. Player 1 stays
the script driver; player 2 is a real second ped who walks, aims, shoots and rides
along, but never owns a mission.

## Freeroam only: co-op does not run during missions

Co-op is **off while a mission is running.** This is a hard boundary, not a
degradation.

It is worth being clear about why, because it looks like a limitation and is
actually the thing that makes the rest affordable:

- It turns a **degraded** mode into a **bounded** one. Letting co-op run through a
  mission means player 2 is a bodyguard for the whole campaign, which is the part
  of this design with the most ways to be subtly wrong and the least fun.
- It removes a whole class of risk rather than managing it. Mission scripts, the
  cutscene cameras, mission-failure respawn and every `PlayerInFocus` assumption
  inside a scripted sequence all stop mattering, because none of it runs
  concurrently with a second ped. The camera already yields to cutscenes; that
  patch exists because of a structural conflict, and this removes the conflict.
- It is what SA MP actually did. There was no co-op campaign; co-op was freeroam
  and community missions.

Cost: nothing, since player 1 owning scripts and player 2 being a passenger was
already the design. This just makes it explicit rather than letting the mode run
in a state it was never designed for.

Co-op missions remain possible later as **custom `.scm` missions**, which drop
straight in from the SD card, and which get co-op only once they opt in.

### What this makes the top remaining risk

**Player 2 dying in freeroam.** Nobody respawns them: respawn is driven off
`PlayerInFocus`, which stays 0, so a second ped that dies is a corpse the camera
then frames for the rest of the session. In mission-free co-op this is a *likely*
event rather than an edge case, so it needs explicit wiring before co-op is
usable rather than after.

Not yet located: the respawn symbol itself. Three searches came back empty, so
this is recorded as an open question rather than a known gap.

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

## Revision after studying the PS2 SA co-op

Gate 0 shipped (`d45ff871`) and is correct as far as it goes, but studying what
SA MP actually does says five things, four of which are changes.

**Control ownership is dynamic — this is SA MP's best idea and Gate 0 lacks it.**
Only one player *has control* at a time. The other is a passenger/assistant who
can move and shoot but does not drive, and a button hands control over. Gate 0
hardcodes player 1 as the camera subject, which is the static version of that and
is worse: whoever is not driving is a passenger *by definition*, and the camera
never goes where the action is.

This is a separate axis from script ownership. **Scripts stay with player 1** —
that is unchanged, and SA MP agrees with it, having shipped no co-op campaign at
all. What follows the controller is the camera, the vehicle, and pickup
attribution.

**The camera follows the controller, not player 1.** Follows from the above.

**A plain midpoint fails once the players separate.** At 30m the average sits
halfway between them and the controller sits on the very edge of frame. SA MP
guarantees both are visible. So: weight the midpoint toward the controller, and
past a separation threshold follow the controller alone.

**The ped must face its reticle.** It does not yet. The ped keeps facing
wherever it was walking while the dot is elsewhere, so shooting looks broken even
though the ray is correct. The engine's aim state wants driving the upper body
and weapon from the reticle position.

**The pitch was probably too shallow.** 41 degrees was picked for legibility of
the 3D, which is the wrong priority in a design where aiming *is* a reticle. The
steeper the camera the closer the screen-to-ground mapping is to linear — near
vertical is nearly orthographic — so a shallow angle compresses the horizon and
makes aiming at distance twitchy. **Try 55–60 degrees.** The instinct behind
"semi-topdown" was better than the number that was first put on it.

### Lock-on already solves aiming, and it never touched the camera

`MODE_AIMING` is **commented out** (`Cam.cpp:235`) — the general weapon-aim camera
does not exist in this engine. Lock-on is `m_pPointGunAt` / `m_bHasLockOnTarget`,
both members of **`CPlayerPed`**, and it aims the ped. Only the sniper scope and
the rocket launcher get a camera mode of their own.

Two consequences, and they reorder the work:

**Lock-on is camera-independent, so it survives the fixed camera untouched.** It
carries aiming in co-op with no changes at all. That makes the assist redirect
below a *polish* item for the free-aim case rather than the thing that makes
aiming viable, which is how it was first described.

**Everything per-ped comes free with the second ped.** Lock-on, weapon state,
`m_wepAccuracy`, health and armour are all `CPlayerPed` members rather than
globals or anything keyed on `PlayerInFocus`. The moment player 2 exists as a
`CPlayerPed`, it has all of it. Pickups and mission triggers are the opposite
case -- those *are* keyed on `PlayerInFocus` and stay player 1's.

### This reorders the gates

Gate 1 (input) was placed before Gate 2 (ped) on the reasoning that input is the
blocker. That is backwards. **The ped is where the unknowns are** and it unlocks
every per-ped system; input is mechanical plumbing that drives systems which will
already work. So: Gate 2, then Gate 1, then the assist redirect as polish.

### Framing follows the lock target, not just the players

Pull-back on player separation is the wrong target. If player 1 is locked onto
something across the map, a camera framed only on the two peds shows your own
character shooting at something you cannot see -- precisely the failure the fixed
camera was meant to remove. The rule becomes: weight the midpoint toward the
controller, and widen to include **each player's current lock target**.

### Pitch: pull back, do not steepen

This has now been argued twice and been wrong twice. It was first pushed steeper
(55-60 degrees) for screen-to-ground linearity in free aim, then partly walked
back once assist turned out to carry that case. With lock-on carrying aiming
outright, pitch is purely a framing question -- how well both players and their
targets read -- and that argues for pulling back rather than steepening. Choose
it by looking at it; the prior is now the opposite of the first guess.

The most important consequence of the fixed camera, and one Gate 0 does not
handle.

`CAimAssist::Process(Source, Front, Up, FOV, AlphaOffset, BetaOffset)` is called
from `Cam.cpp:1539` inside the follow-ped path, and what it writes is then applied
straight to the camera's look angles:

    CAimAssist::Process(Source, Front, Up, FOV, AlphaOffset, BetaOffset);
    Alpha += AlphaOffset;
    Beta  += BetaOffset;

So aim assist **turns the camera**. The pointer is fine adjustment layered on top
of a snap that is already doing most of the work — the crosshair turning red
(`Hud.cpp:295`, `CAimAssist::IsEngaged()`) is the tell that it has engaged.

That makes two things true at once:

1. **Reticle-only aim is viable precisely because the snap exists.** It is not a
   downgrade. This is the strongest argument for the whole design and it was
   missing from the first draft of it.
2. **A camera that takes no input loses aim assist entirely.** Gate 0 as shipped
   loses it, which is a far bigger loss than losing camera steering — it is losing
   the mechanism that was doing most of the aiming.

The fix is to redirect assist from the camera to the reticle. A target's screen
position is just that target projected onto the image plane, and the reticle is
already screen-space, so assist's magnetism becomes "ease the reticle toward the
target's projected position". Same snap, camera stays still, and it comes free
per player because each has their own reticle.

**This also weakens the steeper-pitch argument above.** It was argued that a
steeper camera gives a more linear screen-to-ground mapping and so finer aim. If
assist does the heavy lifting and the pointer is fine adjustment, the precision of
that mapping matters much less, and 41 degrees is more defensible than the note
above claims. The pitch should be chosen by how well the 3D reads, not by aiming
linearity.

### Also worth copying

**Drop-in / drop-out rather than on/off.** SA MP's second player could leave and
rejoin freely. On this port that falls out of the toggle having three states —
off, joined, and *requested* — with the join only completing when a second
controller is actually present.

### What SA MP confirms about the rest

Player 1 owning scripts, freeroam plus custom missions only, the non-controller
as a free-aim role, per-player health/armour/weapons, and pull-back-on-separation
are all what SA MP settled on after years of people attempting it. That is
evidence rather than taste, so none of it is being changed.

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

### Gate 1 — a second controller: bigger than it looks

`WPAD_CHAN_0` is hardcoded in nine places. Threading a channel through them is the
easy half and is pure mechanics.

**The hard half is that the port has exactly one input sink.** `WiiPadState.cpp`
holds `g_keys`, `g_mouse`, `g_moveX/Y`, `g_lookX/Y` and the cursor in a single
anonymous namespace, and every pad's state is funnelled into one
`lwjgl::Keyboard` and one `lwjgl::Mouse` -- process-wide singletons that `Pad.cpp`
and `Frontend.cpp` read directly. `CapturePad(padID)` does give each pad its own
`CControllerState`, so *buttons* are already per-pad. The pointer, the cursor and
the reticle are not.

So a second Wiimote needs all three of:

1. the channel threaded through the nine `WPAD_CHAN_0` sites,
2. per-pad `g_keys`/`g_mouse`/cursor/reticle state, and
3. **a per-pad keyboard and mouse sink** -- which the engine does not have, because
   that is the interface `Pad.cpp` and the frontend read.

Step 3 is the actual work, and it is not a Wii-port change: it is a change to
how the engine receives input. Until that exists, a second controller is not a
`WiiPad.cpp` edit.

Two remotes today do not merely collide, they **overwrite**: both read channel 0,
so one player drives two peds.

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