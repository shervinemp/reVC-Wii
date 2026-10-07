# COOP-4 — three and four player couch co-op

*Scoping document. Branch `coop-4`, based on `couch-coop` at `ba910797`. Not implemented yet.*

## Verdict

Three and four players are both feasible, and the cost is the same for either: the work is
deleting the *pair* assumption, not adding a player. The engine turned out to be more
four-player-ready than expected — the player types, population, and save systems handle four
today — and the pair assumptions are localized to the co-op layer, the pad layer, the camera,
and the HUD.

The two real limits are not code: one camera on one screen, and the frame cost of four skinned
player peds.

## What the audit found

### Already four-player-ready (no work)

- `PEDTYPE_PLAYER1..4` exist (`src/peds/PedType.h:7-10`) and `PedType.cpp:129-132` parses all four names.
- `CPed::IsPlayer()` already returns true for all four types (`src/peds/Ped.cpp:7743-7745`).
- Ambient spawning already excludes all four player types (`src/peds/Population.cpp:224-227, 286-289`).
- The save only ever stores `PEDTYPE_PLAYER1` peds (`src/core/Pools.cpp:514-517, 528-531`) and only
  creates the PLAYER1 ped on load (`575-578`) — partners are excluded by ped type, so a four-player
  session saves exactly what a two-player one does. No save work.
- Each player ped updates its own wanted (`src/peds/PlayerPed.cpp:1817`), so a partner's wanted
  decays independently already.
- Every `NUMPLAYERS` loop site is nil-safe today — they all run with slot 1 empty whenever co-op is
  off: `Game.cpp:310/537/755/833/896`, `PlayerInfo.cpp:731`, `PlayerPed.cpp:207`, `Replay.cpp:1341/1452`.
- Everything keyed to `CWorld::PlayerInFocus` (scripts, HUD, stats, camera focus) stays on player 1
  by definition and does not change.
- Mission/cutscene suspension is player-count-agnostic (`CCoop::SessionBlocker`).

### The pair assumptions (the work)

1. **Engine slots.** `NUMPLAYERS = 2` (`src/core/config.h:35`), `CWorld::Players[NUMPLAYERS]`
   (`src/core/World.h:62`). Raising it to 4 is the only engine-wide change; the audit above is the
   reason it is small.

2. **Fixed slot constants.** `LEAD = 0`, `PARTNER = 1` (`src/core/Coop.cpp:55-56`) and the ~28 sites
   that use them (session spawn/remove, leash, regroup, aim, arsenal, wanted, death).

3. **Single-partner state.** `s_aim[2]` (`Coop.cpp:187`), `s_carry`, `s_joined`/`s_padHere`/`s_padEver`,
   `s_announceJoin` (`191-213`), `s_leadWanted`/`s_partnerWanted` (`280-281`), `ms_bPartnerFocus`
   (`Coop.h:102`), `s_poolAmmo` (stays one pool; per-partner spend caches).

4. **Pad layer.** One `PAD_COOP` slot (`src/core/Pad.h:154`), `s_devices[2]` with
   `PLAYER_ONE`/`PLAYER_TWO` (`src/wii-port/WiiPad.cpp:201-202`), `resolveDevices()` assigning exactly
   two players (`~1064-1100`), `WiiPadCapture` mapping pad to device (`~1501`), `s_flick[2]` (`~230`),
   `s_partnerAimX/Y`, `CCoop::ReportPartnerPad`/`ReportPointer(player, ...)`, the per-pad capture call
   (`src/core/Pad.cpp:1803-1804`), the padID mapping in `CPad::Update` (`Pad.cpp:2337`), the frontend
   pad pick (`Pad.cpp:134`).

5. **Player-slot helpers that answer 0/1.** `GetPadIndexFromPlayer` (`PlayerPed.cpp:66-72`),
   `LeaningSlot` (`src/vehicles/Vehicle.cpp:73-75`), `JackSlot::Of` (`src/peds/Ped.cpp:2646`),
   the car-entry gate in `PlayerInfo.cpp:611`, the `PEDTYPE_PLAYER2` clause in `Weapon.cpp:1481`.

6. **Camera.** Pair midpoint (`src/core/Cam.cpp:1218-1228`), partner focus (`Cam.cpp:1246`), and the
   zoom gains keyed to the pair's separation.

7. **HUD.** `DrawCoopMarks` loops `i < 2` (`src/renderer/Hud.cpp:241-245`) with one pip look.

8. **Pickups.** One partner asked after player 1 (`src/control/Pickups.cpp:1005-1029`), one collect
   button (`996`).

9. **Menu/INI.** One partner skin: `WiiCoopSkin` (`WiiPad.cpp:1276`), `coopSkins[]` and the select
   row (`src/core/MenuScreensCustom.cpp:132,165`), `CoopSkinAfterChange` (`181`).

10. **Shake/rumble routing.** `PAD_COOP` shake lines (`Explosion.cpp:360`, `Weather.cpp:208`), rumble
    on channel 0 (`WiiPad.cpp:1735`).

## Design decisions (proposed)

- **Slots.** `NUMPLAYERS = 4`; partners are `PEDTYPE_PLAYER2/3/4` in slots 1..3. `PlayerInFocus` stays
  0. The save needs nothing.
- **Pads.** `PAD_COOP` stays partner 1; add `PAD_COOP2`/`PAD_COOP3`. Slot to pad: 0 → `PAD1`,
  1..3 → `PAD_COOP + (slot - 1)`. Nothing else reads the new slots, so the PAD2 debug-pad trap
  documented in `Pad.h` stays avoided.
- **Devices.** Assign connected controllers to player slots in order — GameCube ports first, then
  remotes with a Nunchuk or Classic controller, the same rules the boot screen holds player 1 to.
  Each partner joins and leaves exactly the way partner 1 does today (their controller appears or
  goes away).
- **Tether.** Star: each partner is leashed to the lead with the same wall (`kTetherStart` 18 /
  `kTetherMax` 24). The camera's span becomes the maximum pairwise distance — with two players that
  is exactly today's separation, so the two-player feel does not change.
- **Camera focus.** `ms_bPartnerFocus` (a bool) becomes which partner's car the shared camera
  follows — the last partner who pressed their camera button.
- **Arsenal.** One pool (player 1's) mirrored to every partner, per-partner spend caches. The pool
  drains N-1 times faster with N players; that is deliberate (the pair shares its guns), and worth
  revisiting only if it feels thin on hardware.
- **Wanted.** Rises mirror to the maximum across all players; decays stay per-ped.
- **Money/pickups.** Still routed to player 1 (one wallet); health/armour/bribe to the collector.
- **Skins.** One skin per partner. The menu gets PLAYER 2/3/4 SKIN rows; the INI grows `CoopSkin2`
  and `CoopSkin3` (partner 1 keeps `CoopSkin`).
- **HUD.** One pip and health bar per player, four colours.
- **Reset semantics.** Player 1's wasted or busted resets the whole party (all partners removed
  without carry, as the two-player build does now for one). A partner's own death respawns that
  partner beside player 1 with the pair's set.

## Stages

1. **Engine slots.** `NUMPLAYERS = 4`; verify the nil-safe loops; no behavior change with two players.
2. **Device layer.** Probe/assign/capture four devices; per-player flick, pointer, rumble; join
   callbacks carry a partner index (ignored until stage 3).
3. **CCoop to N.** The bulk: loops and per-partner state for session, join/leave, leash, regroup,
   arsenal, wanted, pickups, death; pad mapping; help text.
4. **Camera for N.** Centroid target, max-pairwise span; identical to today with two players.
5. **HUD, menu, INI.** Pips, skin rows, join prompts.
6. **Hardware tuning.** Frame time with four peds, tether/zoom feel, pool drain rate.

## Risks / unknowns

- **Frame time.** Four skinned player peds share one clump geometry, and the vertex-drain fix waits
  per duplicate draw — up to three `GX_DrawDone` per frame instead of one. Measure before trusting;
  if it bites, batch the drain per geometry per frame.
- **One screen.** At a 24 m tether a four-player spread can force a ~40 m view (the zoom already caps
  at `kCoopMaxDistance` 40). Expect to tighten the tether or the zoom for three or four.
- **A pair asymmetry worth deciding on.** `PlayerInfo.cpp:611` lets player 1 ride in the partner's car
  but not the reverse. With N this should become symmetric — any other player's car with a driver is
  not enterable, except a fellow player's — or stay lead-only. Pick during stage 3.
- **Hardware.** Four remotes need four Nunchuks (or GameCube pads); the join rule stays "something to
  walk with".

## Gate

Verify `couch-coop` at `ba910797` on hardware before stage 3 starts. Any bug found in the arsenal,
leash, or aim systems lands in code that stage 3 rewrites; starting early means fixing it twice.
Stages 1 and 2 are behavior-preserving and can go first regardless.
