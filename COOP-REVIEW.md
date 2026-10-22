# Co-op review: coop (2026-10-18)

Scope: everything the multiplayer work changed since 352ca4b4 (about 8,600 added lines).
Nothing was built or run. Five reviewers read the code, one area each, and I read the menu,
HUD and text changes myself.

**Third pass (2026-10-21, an audit of the co-op paths for cases, interactions and physics;
read from the code, nothing seen on a TV).** Four reviewers read the shared camera
(`Cam.cpp`), the minigame vehicles and props (`CoopModes.cpp`), the session and player
interactions (`Coop.cpp`) and the engine's treatment of a partner (`PedFight.cpp`,
`PedAI.cpp`, `Vehicle.cpp`, `Weapon.cpp`, `Script5.cpp`). Every finding below was re-read
and confirmed in the source before it was fixed; two claims in the audit were wrong and
were dropped (the "partner's boat takes double the damage" is backwards — it takes half;
and the "race arms itself after a save load" does not reproduce, because the clock is only
zeroed at start-up). Fixed in this pass, none of it run on a console:

- **The cop shift never opened a case.** [V] `caseActive`, `spawnPending` and
  `UpdateSpawnRequest` formed a closed cycle: the request was only run from `UpdateCase`,
  which only ran when a case was already open, and a case was only open once the suspect
  existed, which only happened when the request ran. The shift armed, and then did nothing,
  for ever — no suspect, no clock, no pay. The structure has been there since the shift was
  written (`3231e712`), so every later shift change was untested. `Update` now drives the
  request. The shift also had no re-arm timer, so forfeiting it re-armed it in the same
  frame; it has one now, like every other mode.
- **A barrel blast was player 1's blast.** [V] `CWeapon::BlowUpExplosiveThings` named
  `FindPlayerPed()` as the culprit, so with friendly fire off a partner's barrel hurt
  player 1 — the culprit and the victim were the same ped, which the rule cannot block —
  and spared the partner. The shooter is the culprit now, in the bullet path, the shotgun
  path and the chain reaction.
- **The Rhino's turret followed the wrong player.** [V] It keyed off
  `FindPlayerVehicle()`, which is the vehicle player 1 is *in*: with player 1 riding along
  in a partner's Rhino the turret was driven by player 1's pad. It is the driver's now.
- **A partner's shotgun turned the victim on player 1.** [V] `FireShotgun` said
  `ReactToAttack(FindPlayerPed())` where the instant-hit path says `shooter`.
- **A partner's boat was twice as tough as a party car.** [V] `CBoat::ProcessControl`
  keyed off `FindPlayerVehicle()` where the car uses `IsPartyVehicle`: a partner's hull
  took half the collision damage player 1's did, while a party car takes the same as
  player 1's, and the driver the engine let complain about the noise was the partner's.
- **The derby and the RC race could not end.** [V] The derby capped a round but not a
  match, and a round that ran out of time awarded nobody, so it looped for ever; the race
  capped the grid but not the racing. Both have a match cap now.
- **A partner's skin that would not stream made them late to every arrival.** [V] The
  give-up cleared the wait, which is the same value as "not waiting at all", so the next
  arrival began the whole six seconds again — and the one after that.
- **The camera framed the dead.** [V] A downed partner's body was in the centroid and in
  the separation that sets how far back the camera sits, so the view centred on a corpse
  and the two players who could still act became specks.
- **The party could spread further than the camera can show.** [V] The wall was a star
  and the camera zooms out to 40 m: two partners on opposite sides of player 1 could be
  48 m apart and one of them went off the screen. Partners are held to each other now, by
  a wider band.
- Smaller: a camera focus outlived the partner who asked for it; a revive put a partner
  back at the raw coordinate where they died, with none of the checks every other
  placement uses; a bike beside a partner cancelled the whole "take a car of your own"
  search; the HUD's down bar counted a different clock from the bleed-out; an RC racer
  who never had a toy was given a gate and a lap bar; a partner's katana could never
  decapitate; a gang-7 ped did not put its hands up at a partner's gun; a partner's mugging
  produced no money; a partner's bat had no motion trail; a partner's Rhino registered no
  explosion events; and a mode's own vehicles were handed to the traffic system when the
  mode ended — which also left the traffic counters wrong, because those cars were never
  in the traffic system to begin with.

**Fourth pass (2026-10-21, the rest of the list from the third pass).** Fixed here, none of it
run on a console:

- **The camera's wall-clip room was measured in the wrong frame.** [V] The room behind the
  camera was measured from each player and the loosest answer won, but the camera hangs off
  the middle of the party: with the party strung out along the view, the front player's
  generous answer put the camera ten metres inside a building. The room is now measured from
  the middle and each player's answer is shifted by how far along the view they stand.
- **A player above the camera aimed behind it.** [V] The reticle's level plane is the
  player's own chest; when the player is above the camera the distance to that plane is
  negative, the ray was traced backwards out of the back of the view, and the shot left the
  muzzle pointing away from the screen. The ray is now followed only as far as it goes in
  front, and the shot goes the way the reticle points.
- **A partner's ram did a fraction of player 1's.** [V] The 1.5x ram bonus was keyed on
  `FindPlayerVehicle()`, so a car rammed by a partner took a fraction of the damage one
  rammed by player 1 took. The crash voice line was keyed on it the other way round: a
  partner's car announced every shunt and player 1's did not. Both are the party's now.
- **A mode's payout was invisible.** [V] The money goes to the party's one wallet, as the
  wanted level and the arsenal do, and the result said only `P2 WINS` — which read as a
  win that paid nothing. The result says what it paid: `P2 WINS   PARTY +$500`.
- **The second Predator turned up a leg late.** [V] The index was the leg that had just
  finished, which was the slot the first Predator had already taken, so the second boat
  only ever appeared for the last drop.
- **Two RC toys could be dropped on the same square.** [V] The toy goes 2.5 m in front of
  its racer, and two racers in a knot had toys shorter apart than a toy is long: the
  collision resolver threw them apart on the first physics step and the grid never
  settled. A toy whose square is taken is put to one side of it.
- **Player 1's controller dying did not stop the game.** [V] The pause button, the HOME
  button and the Quit in the pause menu are all on that controller, so a game that carried
  on without it was a game somebody had to stop at the wall while the world kept going.
  Three seconds of silence now pauses it; the pause is the recovery, not a punishment.

Three items from the third pass turned out to be already fixed and were dropped: friendly
fire through vehicle explosions (`SparedByFriendlyFire` in `KillPedsInVehicle`, added in
`25906e4d`), the pips for a player in a vehicle (the co-op work marks over the vehicle), and
the camera's "keep the heading it already had" branch (entering the shared camera is a
transition, and a transition sets `ResetStatics`, so the branch is reached).

**Fifth pass (2026-10-21, the last of the redesign's content).** Fixed here, none of it
run on a console:

- **The modes had one venue.** [V] Every mode but the dance-off and the run started on the
  airport apron, so the party learned one place and every mode felt like the same
  afternoon. The venues are now named and different: the airport (the derby, the cop
  shift), the golf course (the Caddy), the stadium car park (the toy race), the docks
  (the hill), Ocean Beach (the dance-off, cops and robbers), the dockyard (the run).
  The airport and the beach are known spots; the others are bases the game walks out
  from in rings, taking the first square that is dry, flat and clear of the size the
  mode needs, with the airport as the fallback for anything it cannot find. The
  ready-up card names the venue before anybody has moved.
- **The camera's collision tests are not done every frame.** [V] Measuring the room
  behind the camera is up to four line-of-sight tests of forty metres, every frame --
  the most expensive thing the shared camera does. The camera eases towards the
  answer anyway, so it is measured every third frame now and held in between, and
  measured at once whenever the view moves to a new place.
- **The cop shift's whole path was read again now that it runs.** [V] The vehicle and
  ped clean-ups use the engine's own order -- `CWorld::Remove` before the delete, or
  the deleted vehicle stays in the world's lists -- and the suspect's spawn no longer
  deletes a player who is driving nearby.

**Status (2026-10-18):** everything below is fixed except the list under "Still open". All of it
compiles (`-fsyntax-only`, the Wii build's flags, all 250 game sources, no warnings); none of it has
been run on a console, and the PC build was not compiled. Item 3 turned out to be wrong and needed
no fix. A few fixes chose between two readings of the intent; the commit messages say which.

**Still open**
- 12: player 1's controller dying does not pause the game (not built). `updateFlick` and
  `NunchukLean` still read `exp.type`, on purpose (the probe could feed them zeros).
- 18: two RC toys are still placed per party member, not per jumper (a design call), and money
  from a mode still goes to player 1 whoever wins.
- 33: the shared camera's four line-of-sight tests per frame are not throttled (no way to see a
  pop without hardware).
- 28: a partner killed while driving leaves the car in `STATUS_PLAYER` (`PedFight.cpp` ~3236).
- Vehicles: the crash voice line and the 1/1.5 ram bonus still treat only player 1's car as the
  player's.
- `Ped.cpp` ~9318 (NPC jump heading under pointer controls) differs from stock on purpose; it is a
  bug fix, not co-op.
- Camera/HUD layout of the new mode headlines, countdowns and bars, and every timing constant the
  fixes introduced (wall hold 2.5 s, 5 s on foot before a new RC toy, 20-scan controller grace,
  6 s skin wait, 40 m regroup cutoff), want a look on a console.

**Second pass (2026-10-18, a UX and polish audit; read from the code, nothing seen on a TV)**
Done: notices that stay up long enough to read, "press A to join", the pause menu from joined
pads only, a note for a bare Wiimote, a join buzz, messages for being brought back / waiting for a
seat / the wall / the mission pause, per-controller camera and leave tips, the GameCube Z+R chord
masked, low battery and a "?" for a lost controller; badges with numbers, outlined reticles that
keep their colour on a target, a luminance spread in the four colours, a DOWN bar and a revive bar;
the cops & robbers bust lockout, the cop shift's streak and result, readable dance chips and a
slower beat, lane tags and cells, first-time explainers, the derby's "ramming only" announced.
Open, and not for code alone:
- Design calls left to the owner: friendly fire defaults on; every partner defaults to player 1's
  skin; co-op is reachable only from the pause menu; the smuggling run can never start
  (`FindCrewBoat(2)` needs two players in one boat and partners cannot board boats). Settled since:
  the Shared Wanted toggle did almost nothing (both settings put a partner's crime on player 1 and
  followed player 1's drops), so it is gone and the party always shares one wanted level, the one
  player 1 holds (design doc, "Smaller rules"). The menu row, its ini key and its text are removed;
  a stale `SharedWanted` line in an existing `reVC.ini` is never read.
- Minigame arming had no guard against an accident, and a glued pair could trade busts for about
  7.5 s a round. Settled since (code only, nothing run): the toy race, the dance-off and cops &
  robbers do not arm while anyone in the party is wanted; cops & robbers needs the nod held 0.6 s
  (a tap, or a press with a car in reach, starts nothing); whoever picks the case up cannot be
  busted for 2 s, beside the 2 s lockout on the busted player.
- The smuggling run, settled since the same way (code only, nothing run): it arms on player 1 at
  the wheel of a boat and the horn, and the rest of the party are given boats to drive beside the
  lead (`CCoopModes::EscortBoat`, seated by `SeatPartnerAsDriver` in `SpawnPartner`), because a boat
  has one passenger seat at most, drawn on top of the driver's, and a boat passenger has no exit.
  Unverified on a console: the seating, the regroup keeping passive escorts near a fast lead boat,
  and the hand-back at the end (`CCoop::Suspend`). Design doc, "Minigames".
- The GameCube chord's first button still fires its own action for a frame or two.
- The battery flag is the remote's own status flag and may lag; nobody has watched it.
- The explainer flags reset whenever the session pauses (a mission, a cutscene), so one returns
  afterwards.
- HUD badges can overlap other things when the party spreads out, and nothing clamps them. At
  640x448 (4:3): the top-right cluster of clock, money, health, stars and weapon is x 392-605, y
  20-105; the radar x 34-128, y 328-418; the help box x 30-234, y 26-100; subtitles x 148-612, y
  348-391. A badge stack reaches about 26 left, 12 right, 14 above and 22-31 below its anchor, and
  the HUD proper draws over it.
- Everything above that is a size, a colour, a position or a pace needs the real TV.

Tags: **[V]** the code was re-read a second time to check the claim. **[R]** one reviewer
traced it and it was not re-checked. **[S]** suspected: plausible but not closed.

## A. Crashes and states that break the game

1. **A partner deleted while driving leaves a driverless vehicle that is still a player's.** [V for the boat] **FIXED**
   (`RemovePartner` now abandons the car it was driving, as the engine's own removal does)
   `Coop.cpp:709-752` (`RemovePartner`) does a bare `delete`; `~CPed` (`Ped.cpp:382`) only nils
   `pDriver` and never sets `STATUS_ABANDONED`.
   - Boat: `Boat.cpp:186-194` runs `DoDriveByShootings` in `STATUS_PLAYER`, which casts a nil driver
     and calls `GetPlayerInfoForThisPlayerPed()` (`Boat.cpp:1417`). Crash.
   - Car: gas and steering stay at their last values, so it runs away (`Automobile.cpp:545-556`).
   - Heli or bike: `GetPadFromVehicleDriver` returns pad 0, so player 1's sticks fly it.
   - Triggers: a mission or cutscene starting, player 1 wasted or busted, the controller gone 6 s,
     a revive, a regroup with no free spot.
   - Fix: call `RemoveDriver()` and zero the controls (or `FlagToDestroyWhenNextProcessed`) before
     the delete. Guard `CBoat` `STATUS_PLAYER` on `pDriver != nil`.

2. **Voodoo hydraulics crash after a partner gets out.** [V by reading] **FIXED**
   (`SpecialColModelOwner` finds the player the car is registered to when it has no driver)
   `Automobile.cpp:268-273` `SpecialColModelOwner(nil)` falls back to player 1's special collision
   model. A partner steals a Voodoo and leaves; the car is abandoned but still flagged
   `bUsingSpecialColModel`; `HydraulicControl` (`:3504`, also `:807`, `:834`) reads `lines[i]` from
   player 1's empty model. Wrong model if player 1 has used hydraulics, null read if not.
   Fix: find the owner by scanning `Players[i].m_pVehicleEx == this`.

3. **Cops and Robbers: radar dereferences a deleted ped.** [R] **WRONG, no fix needed.** `~CPed`
   (`Ped.cpp:373`) calls `CRadar::ClearBlipForEntity(BLIP_CHAR, ...)` for the ped being deleted, so the
   case blip is already gone when `CCoop::Update` deletes the carrier. The original text follows.
   `CoopModes.cpp:1941-1953` clears the carrier's blip only if the ped still exists;
   `Radar.cpp:478-505` (`Draw3dMarkers`) has no nil check. A partner carrying the case whose pad
   drops, whose skin changes, or when a mission starts gets deleted by `CCoop::Update` before
   `DropCase` clears the blip. Fix: keep the blip id and `ClearBlip(id)`.

4. **Any Wiimote's POWER button quits the whole game.** [V] **FIXED**
   (only player 1's remote quits; anyone else's is switched off from `WiiPadScan`)
   `wii_game.cpp:599` `onWiimotePowerButton(s32)` ignores the channel and calls `HandleExit()`. A
   partner holding POWER to turn their remote off ends everyone's session, with no prompt (HOME has
   one). Fix: ignore other channels, and send player 1's through `RequestQuitPrompt()`.

5. **PC build break.** [V] **FIXED** (the constant is now outside the ifdef; not compiled on a non-Wii target here)
   `Weapon.cpp:2744` uses `kDriveByAimCone`, declared only under `#ifdef NINTENDO_WII` (`:49`). The
   preprocessor depth at that line is 0. Fix: move the constant out of the ifdef.

## B. Controller assignment and input

6. **Players follow connection order, not the controller.** [V + R] **FIXED**
   (a partner keeps their controller's slot; a controller gone for 20 scans is released; GameCube port
   bits get the same grace; new controllers take the first free slot)
   `WiiPad.cpp:1095-1129` repacks partners every scan. P2's remote dying or its Nunchuk being pulled
   moves P3's controller into P2's ped; 6 s later P4's ped is deleted with "player 4 left" while
   everyone is playing. A GameCube pad plugged in mid-game shifts every Wiimote partner down a slot.
   Player 1 is welded to channel 0. One glitched `PAD_ScanPads` frame swaps player 1 [S].
   Coop's per-index `s_joined` / `s_padHere` / colour assume a stable index (`Coop.cpp:1635-1660`).
   Fix: bind each device to a slot when it joins, keep the binding while present, debounce ~0.5 s.

7. **`FrontEndPad()` swallows pauses and ignores partners' sticks.** [V]
   `Pad.cpp:132-140` returns the first partner pad with any fresh press, including fire, jump and
   weapon cycle. `SwitchMenuOnAndOff` then asks that pad about Start, so player 1's one-frame Start
   is lost whenever a partner has an edge, and partners win simultaneous presses. In menus,
   `CPad::GetAnaloguePadUp/Down` are static and read pad 0 (`Pad.cpp:3282`), so a partner's stick
   never moves the menu and player 1's stick can.
   Fix: in game check Start on every pad; in menus switch only on a menu-relevant edge, prefer pad 0.

8. **GameCube rumble is overwritten by a partner's STOP.** [R, read from libogc disassembly]
   `WiiPad.cpp:1809-1810` calls `PAD_ControlMotor(device.channel, STOP)` for Wiimote players using
   the Wiimote channel as a GC port; last write wins. Player 1 on a GC pad in port 0 plus a partner
   remote on channel 0 loses rumble every frame. Fix: `WPAD_Rumble` only for Wiimotes,
   `PAD_ControlMotor` only for GC.

9. **Rumble leaks.** [R] `ShakeDur` doesn't decay in pause and `StopPadsShaking()` zeroes pad 0 only,
   so a partner buzzes through the whole pause (`WiiPad.cpp:1782`). A player whose device vanishes
   never gets `WPAD_Rumble(ch, 0)` (`:1806`). Unjoined partner remotes are rumbled, and
   `Explosion.cpp:360` / `Weather.cpp:209` shake every coop pad even with co-op off.

10. **The remote speaker is player 1's channel 0 for everything.** [R]
    Only gunshots check `WiiPadRemoteIsPartners()`. Radio static, phone rings, wake and mission
    calls play on channel 0 whoever holds it (`WiiSpeaker.cpp:34,325,366,382`).

11. **A GameCube pad has no Select.** [R] `captureGameCube` (`WiiPad.cpp:590-645`) never sets it, so
    a GC player 1 can't cycle the shared camera framings and a GC partner can't claim camera focus
    (`Coop.cpp:1594`, `:1612`).

12. Low: `WiiPad.cpp:1636` and `:1149` test `exp.type` while capture uses the probe, so they disagree
    for a frame at a Nunchuk hot-plug. Nothing pauses the game if player 1's controller dies.

## C. Minigames (`CoopModes.cpp`)

13. **Dance-off beat is broken.** [V] `:1738-1744`: `kDanceBeatFastMs - kDanceBeatSlowMs` is
    `uint32` (420 - 700), which wraps to ~4.29e9. After the first beat the interval is ~5e7 ms: one
    prompt per player, then nothing for the rest of the 60 s round. Fix: compute as float or signed.

14. **RC race spawns a car every frame for a racer on foot.** [V] `:1502-1514` calls `RespawnRcCar`
    each frame while `car == nil`; it releases last frame's toy and spawns a new Bandit. Dozens per
    second until the racer sits in one; hitches and drains the vehicle pool. Fix: respawn once, then
    wait for entry or a timer.

15. **Speed thresholds are in the wrong units.** [V for the units] `GetMoveSpeed()` is game units,
    1.0 = 50 m/s (`CarCtrl.h:7`). `kStopSpeed = 0.6` is ~108 km/h, `kDropSpeed = 4.0` is 200 m/s,
    and the RC "still" check is 0.6 too. A suspect fleeing at cruise (0.42) counts as "cornered" after
    2 s within 12 m; smuggling drops complete at full speed; the derby arms from a moving convoy.
    Fix: multiply by 50 or use ~0.012 and ~0.08.

16. **"The nod" is also the enter-vehicle button.** [R] Cops and Robbers starts on any press while
    everyone's on foot within 12 m (`:2054-2106`), and the dance-off's "2" prompt walks players into
    nearby cars. Fix: arm on a chord or a button that does nothing on foot.

17. **No on-screen feedback for most modes.** [R] The derby ring is never drawn although the header
    says it is (players are eliminated after 3 s outside an invisible 14 m circle). Countdowns, lap
    counts, drops remaining, dance scores and winners aren't shown; the only text is a one-word help
    title that is skipped if the help box is busy. Money goes to player 1 whoever wins.
    `SetCoordBlip` / `SetEntityBlip` ignore colour (`Radar.cpp:1154-1175`), so the case and getaway
    blips look the same.

18. Other: [R]
    - `RemoveSuspect` deletes the suspect car without checking who's in it (`:530-546`) [S].
    - A wrecked locked loaner never gets replaced (`:1117`).
    - `StartCase` isn't gated by `CoopMinigames`, so turning it off mid-shift keeps spawning
      suspects (`:2344`).
    - Two players jumping within 1.5 s raise four RC Bandits (`TryStartRc`, ~`:1573`).
    - Empty `MISSION_VEHICLE` RC cars and loaners get written into saves (`Pools.cpp:268-281`) [S].
    - Smuggling drops can spawn under the boat and pay out in 2 s (`:804-830`) [S].
    - The boat route claim is false: `DoPathSearch` never reads `vehicle` (`PathFind.cpp:1669`) [S for
      the on-water effect].

## D. Party and session logic

19. **Shared wanted never copies a drop.** [V] `Coop.cpp:333-381` only syncs when a level rises, and
    stars above 1 don't decay (`Wanted.cpp:406`). Party at 4 stars, player 1 sprays and clears, the
    partners stay at 4, and the next small crime puts everyone back to 4. Also makes each partner
    run `FindNewAttackPoints` every frame (about 4x chase cost) [S]. Fix: lower partners when
    player 1 falls, or stop mirroring onto partners.

20. **Friendly-fire off still kills through explosions.** [V for the caller list] The only caller of
    `FriendlyFireBlocked` is `PedFight.cpp:2701` (`CPed::InflictDamage`). A rocket or grenade on a
    partner's car goes `CVehicle::InflictDamage`, then `BlowUpCar`, then `KillPedsInVehicle`
    (`Vehicle.cpp:2419`), which calls `SetDead()`. The design doc's "every path ends up there" is false.

21. **Leash and wall.** [R] A passenger in another partner's car gets the 28 m leash while the driver
    gets 50, so they are deleted and respawned beside player 1 (`Coop.cpp:1172`). The wall holds
    player 1 at 24 m to every on-foot partner, so an AFK or stuck partner pins them (`:1957-1986`).
    The regroup of a driven car can re-fire every frame on a node farther than the leash (`:1797-1862`) [S].

22. **Hitch on every skinned-partner spawn.** [S] `LoadAllRequestedModels(false)` blocks on the whole
    streaming queue (`Coop.cpp:861-866`), on every regroup, hop-in, revive and mission return.

23. Smaller [R]:
    - Carried weapons return at 0 ammo even if player 1 no longer holds that type (`:911-924`).
    - `Init()` doesn't reset `ms_nCameraFocus` (`:1409-1454`).
    - A spawning partner only counts player 1 as a possible driver (`:817-829`).
    - `RegisterReference` returns silently when the 800-entry pool runs dry (`:899`, `:1914`) [S].

## E. Partners treated as second-class in the original code

24. **Bribe and shop pickups don't work for a partner.** [R] A bribe lowers only the partner's wanted
    level, and cops read player 1's (`Pickups.cpp:815`). Armour, health and adrenaline in a shop are
    charged to the partner's `m_nMoney`, always 0 (`:450`).
25. **A cuffed partner can still be killed**, and gang-7 immunity and fireproofing apply only to
    player 1 (`PedFight.cpp:2715`, `Ped.cpp:7100`). [R]
26. **Partners walk through fire** (`Fire.cpp:108`). A partner's bat or fist on a car skips the driver
    reaction, and their flamethrower doesn't scare bystanders (`Weapon.cpp:856, 2361`). [R]
27. **Crimes are judged at player 1's position**, not the criminal's (`EventList.cpp:246`) [S]. Killing
    a partner still adds havoc and media heat, and an NPC killing one bumps `PeopleKilledByOthers`
    (`PedFight.cpp:3179`). [R]
28. Player 1's pickup redirect forces their weapon slot (`Pickups.cpp:458,474,510`). In a burning
    partner-driven car player 1 gets the on-foot flee order (`CarAI.cpp:636`) [S]. Being busted
    in a car may leave player 1's car frozen (`CopPed.cpp:109`) [S].

## F. Vehicles and camera

29. **Rhino turret fires off by the tank's yaw** under the reticle: world heading used where
    hull-relative is expected (`Automobile.cpp:3382-3396`). [R]
30. **With the FreeCam option on, the turret and fire hose freeze in co-op** (`Cam.cpp:65-71`). [R]
31. **Fire truck hose follows the passenger**, not the driver (`Automobile.cpp:3285`). [R]
32. **Player-1-only protections skip partners' cars.** A flipped car loses 4 HP per tick unless it's
    player 1's (`Automobile.cpp:4360`); only player 1's vehicle takes half collision damage
    (`:4526`, `Bike.cpp:2311`). [R]
33. Camera line of sight: up to four `ProcessLineOfSight` calls of 40 m every frame (`Cam.cpp:1392`) [S].

## G. Single-player behaviour that changed with co-op off

The reviewers found no unguarded camera, renderer or shadow change. These are the exceptions: [R]
- `IsAnyPlayerAttack` replaced `damagedBy == FindPlayerPed()` (`PedFight.cpp:2712, 3179`): a
  player-driven vehicle hitting or killing now adds havoc/media; `bOnlyDamagedByPlayer` refuses
  nil-source damage (`:2737`). Fixed machine guns pass the vehicle as `damagedBy`, so a player-flown
  Hunter or Sea Sparrow earns kill credit (`Weapon.cpp:3248`).
- `Automobile.cpp:4073` `if (IsRealHeli() || IsRealPlane()) return;` is unguarded: single-player
  pilots with AIM IN CAR or Drive-By Weapons can't shoot from a Maverick or Dodo.
- `PlayerPed.cpp:1871`: a passenger no longer shuts the driver's door. `Ped.cpp:9318`: NPC jump heading.

## H. UI/UX (my own read)

34. **Vehicles cheat page runs off the bottom.** [V by arithmetic, not seen on screen]
    14 rows from y=78 on a 25 px line puts Back at y=428; the menu frame ends at 426 (398 on the
    right) and the screen is 448. A previous layout was moved up because ~431 was judged off-screen.
    World page (11 rows) puts Back at 353 and is fine.
35. **Pips vanish in vehicles.** `Hud.cpp` `DrawCoopMarks` skips any player with `bInVehicle`, so a
    driving partner's health and ammo are shown nowhere (the HUD is player 1's).
36. **Restore Defaults is half-done.** `Frontend.cpp:5038` resets the pointer toggles and partner
    skins but not Friendly Fire, Minigames or the co-op toggle (it also missed the Shared Wanted
    toggle, which has since been removed).
37. **One help box for every notice.** Join, leave, down, revived, framing and tether messages all use
    `CHud::SetHelpMessage`; simultaneous events overwrite each other.
38. Text: all 106 `WII_*` keys used by the co-op code resolve (fallback table or GXT). Only 10 are in
    `american.txt`. The tether message wording differs between `american.txt` and `Text.cpp`.
    The `PAD_COOP2/3` comment in `Pad.h` is stale.

## I. Where the design doc is wrong

- "Every path ends up in `CPed::InflictDamage`" (friendly fire): false, see 20.
- "Drops decay on their own" (wanted): false above 1 star, see 19.
- "Everything past the 28 m leash comes back": a passenger in a partner's car hits it while its driver
  doesn't, see 21.
- "One partner leaving is nobody else's business": see 6.
- "No partner ever takes a boat, and one can ride in a boat": `FindCarToSteal` (`Coop.cpp:1003`) offers
  boats and `CanRideAlong` (`:668`) excludes them.
- "Rumble works for everyone": false for GC, see 8.
- The RC toy is replaced "at the middle" (header): the code puts it beside the driver. The header
  cites `COOP-4-PLAN.md`, which isn't in the repo. The "path search is handed the vehicle" claim in
  the smuggling header is false.

## J. The sixth pass: the minigames' own state, and the world asking for player 1

Two audits, one of `CoopModes.cpp` against itself and one of the engine against the co-op
design. What they found, and what was done about it.

### Fixed, in the minigames

- **The dance-off's nod was never let go of between rounds.** `s_dance.triangleHeld[]` is written
  only while a round is running (`CoopModes.cpp:4722-4723`) and never reset, so it kept the last
  round's value: a player still holding the nod at the bell started the next round with it stale,
  and every nod prompt read as a miss while the button was down. Reset in `StartDance` and
  `EndDance`.
- **The dance-off's headline said the pot alone.** Every hit pays as well (`kDanceHitPay` a hit),
  so a round that banked hundreds of dollars of hits read on screen as the two hundred it won.
  The pot is now what the round actually paid.
- **Forfeiting a mode in the four seconds after it was decided wiped the result.** `EndDerby` and
  its five siblings clear `finished`, and `AbortActiveMode` reaches them. There is nothing left to
  forfeit then, so the forfeit is refused (`ModeLingering`).
- **The cop shift ate the city's traffic budget.** The suspect's car was cleaned up through
  `CTheScripts::CleanUpThisVehicle`, which takes a mission car off the count; the shift's car was
  never on that count, only the script's `CREATE_CAR` puts one there. Every closed case took a step
  off the mission-car budget and put the random count up, and the traffic system's own clamp made
  the loss permanent. The counters are put back where they belong now.
- **Three menu starts could fail half way and leave the party moved to the venue in cars of the
  mode's making** -- the shift with a police cruiser whose siren was still on (which is also a
  shift waiting to arm itself), the run with its boat, the football with its Bloodring Bangers.
  They are given back now, and the siren goes with the badge.

### Fixed, in the engine

- **The payphone stood the whole session down.** The letterbox the call puts up is the camera's,
  not a cutscene's, so a partner standing at the payphone while player 1 talks on it was taken out
  of the world for the length of the call, with nothing said. They stay now; the things that do
  stand the party down -- a cutscene, the train -- say so (`WII_CCW`).
- **The Pay'n'Spray and the bomb shop are player 1's.** They ask for player 1's car throughout, so
  a partner's own car standing in the garage was not resprayed, not repaired and not even noticed.
  They work on whichever of the party's cars is inside, player 1's first (`CGarage::CarToWorkOn`).
- **The safehouse took the cars inside it and deleted them, and deleting a car destroys whoever is
  riding in it** -- so a partner's car parked there went from under them at the next save. The
  collector took a car's passengers with it, and only ever accepted player 1's car.
- **The garage shoved every car touching it away but the one it was working on**, and the party's
  other car is a car somebody is sitting in.
- **The traffic system was told to keep two stolen cars** (`MAX_CARS_TO_KEEP`), and it is told for
  any player, so a party of four had two of its cars turned back into traffic.
- **A partner walking over a weapon was refused it** for a second and a half after player 1
  pressed anything: the guard read player 1's button and not the body on the pickup. A pickup the
  partner took also stayed gone while player 1 stood near the spot.
- **The crane gave up on a car somebody was climbing into**, and only ever asked about player 1.

### Looked at and left alone

- **"A menu derby leaves a row of Bloodring Bangers in the street."** It does not: the party is
  sitting in them, so the car is handed back to the world with them in it and they drive off. A
  random vehicle is not saved, so the save does not keep it either.
- **"`CCoopModes::Init` forgets the mode's vehicles."** All three of its call sites -- the level
  init, the load and the restart -- are places where the world is rebuilt or torn down, which takes
  the vehicles with it, and the radar is re-initialised on the same paths. Nothing is orphaned.
- **The cheat string is read from pad 0 only.** A developer tool, and the partners' controllers
  are not where cheats are wanted.

### Still to look at

- The minigames' arming gestures are ordinary driving buttons. The football's
  brake and the derby's horn will arm them in traffic. It is never a mode that
  starts by itself -- the card waits for both players to press A -- but the
  card on the screen is an interruption.
- Nothing clears the arena of traffic or pedestrians, so a mode played in the
  middle of the city has the city in it. The police no longer come for it.
- The pay'n'Spray and the bomb shop are player 1's, and the design says so.
  What it does not say is that the party is more than one person.
- The arrest path still ends the hill, where the death path does not: the
  busted sequence is the engine's and it takes player 1 away.

### The modes, played properly

A critique of the eight minigames -- their objective, their fun, and what
Vice City gives them that another game would not -- and what came out of it.

- **The pot now scales with the party.** It did not: five hundred dollars
  whether two people or four, so a four-player game was worth less for each of
  them and a party would pick the two-player game. Two get the base, three get
  one and a half times it, four get twice it.
- **King of the Hill could not be taken.** Two people in the ring made it a
  deadlock, so the only way to play was to sit on the point; the challenger's
  best move was to stand in the ring and refuse to fight, which cost the
  holder nothing. And the score was the *total* time held, which the first
  arrival cannot lose -- the challenger's only way in was the four seconds a
  knocked-down player spends on the ground. The hill is a point now, the
  player nearest it holds it, and the hold is lost when the hill changes hands
  or is left empty, so the bell decides.
- **Caddy football could deadlock on a wedged ball.** Ramming only, and a
  ball under a car is a ball nobody can free. A ball that has not moved for
  six seconds goes back to the middle.
- **Cops & robbers had two rules where one would do.** After a bust the case
  now lies dead for three seconds: nobody picks it up and nobody is pinned.
- **Winning had nothing to show for it.** The money goes into the party's one
  wallet, so there is now a count that is the winner's alone: how many modes
  each player has won, on the ready-up card and the result line. It lasts the
  session, so a loaded game starts a new one.

  The six modes with a winner are the matches; the cop shift and the
  smuggling run are jobs -- they have no winner, they pay per case and per
  drop, and they win no trophy. That is a real gap rather than a design: a
  shift that goes well and a run that delivers all three drops deserve
  something, and the honest answer is that a job's reward is the pay and the
  heat it brings, not a crown.

### The seventh pass: five modes that were not games

The critique above asked whether each mode was fun. It answered, mostly, in
terms of the *rules* -- could the hill be taken, could the ball deadlock, was
the pot worth playing for. Reading the code for those answers missed the
worse problem, which is not that the rules were bad but that in five of the
eight the rules did not decide anything. A mode whose outcome is decided by
the game rather than by the players is a waiting room.

- **The dance-off was not a rhythm game.** The chip came up on the beat and
  stayed up until the next one, and any press of the right button in that
  time was a hit. There was no window and no beat to land on, so the fastest
  way to win was to hold every button down -- except that the dance is armed
  by ducking, so a player holding duck was auto-missing a third of the
  prompts. It also paid up to $1,660 for a minute of button pressing, three
  to five times any other mode, and a round nobody touched paid nothing at
  all.

  The chip now comes up a beat early, a bar inside it drains towards the
  beat, and a press has to land within a window of it that closes as the
  tempo climbs. The press is graded -- perfect, good, early, late -- and the
  grade is on screen for a moment, because without it a missed beat is just a
  missed beat. The game never asks for a button the player is already
  holding, so a mash scores nothing. The hit money is a participation fee and
  the pot matches the other modes.

  There is no music. The port's audio is the game's own stream, and there is
  no sound effect this side of the engine that a minigame can fire on cue, so
  the screen has to be the metronome. A rhythm game that cannot show the beat
  is a guessing game, and that is the ceiling on this mode until the audio
  can be had.
- **Cops & robbers had fixed roles.** The case dropped at the carrier's feet
  and the lock on it was global, so the busted player was the nearest player
  by definition and took it straight back: the same player was the robber for
  the whole match, and the other could win 3-0 without doing anything
  interesting. The busted player is now stood off the case for six seconds
  and the cop takes it, so the sides swap on every bust. The getaway is
  closer (18 m), the grace longer (3 s) and the hold shorter (1.2 s), so the
  robber's job is possible rather than arithmetic.
- **The derby ended a match over a fumble.** Six seconds to find a loaner,
  open a door and drive to the ring, and missing it ended the first-to-two
  match. The round waits now. A tied match used to be won by player one --
  the code took the first player with the best score -- and a match nobody
  won a round of paid nobody anything; a tie is a tie, and a draw pays the
  pot into the wallet without a trophy.
- **The toy race hid the course.** The gates were drawn only once the race
  was running, so the players saw the track at the word GO. They are drawn
  from the grid now. The race was also capped at five minutes and paid
  $300, the least of any mode: it is capped at two and a half and pays $500
  like the rest.
- **The cop shift had eight seconds of nothing between every case**, a
  ninety-second chase with an unarmed suspect in it, and a bust that had to
  be made by stopping a car doing 25 m/s within two metres of you, which was
  not a thing anybody could do. The gap is two and a half seconds, the case
  is forty-five, and the screen says how far off the suspect is.
- **The smuggling run gave the escort nothing to do.** The Predators only
  ever shot the carrier, so the other boat could sit in a corner and still
  deliver all three drops, and a police boat that went down was worth
  nothing. Sinking one now pays, and the screen says so. The escort bonus
  was measured against the lead boat while the screen told the escort to
  hold the carrier's boat, so the two numbers disagreed; it is measured
  against the carrier now. The second police boat was spawned on one frame
  and never retried, so it often never came at all. The crate no longer
  dies with the boat carrying it, and the run no longer ends because the
  skipper climbed into the other boat.

Two rules came out of all of this and now apply everywhere: **a draw pays
the pot into the party's wallet but wins no trophy** -- the money is what the
round earned by being played, the trophy is the prize for winning it -- and
**every match now pays the same $500**, so the party size is the only thing
that decides what a game is worth.

### The advantage of Vice City, honestly

The advantage is the props and the places, not the mechanics. Every objective
here -- last one running, hold the ring, chase the carrier, deliver the crate
-- is genre-generic and would survive a transplant to any open-world game.
What Vice City contributes is a specific toybox: the Bloodring, the Caddy (a
golf cart used as a ball, on a golf course that is a real map), the RC Bandit
(a remote-controlled toy you sit inside, with the driver-not-drawn gag), the
SQUALO and the Predator for a smuggling lane across an island city, the
Cruiser and the helicopter for the police, and six named venues that are each
a short drive apart -- which matters, because the leash is 28 m and a small
map is a precondition for the leash working at all.

The uncomfortable part: the modes that use those props *as their mechanic* are
the ones with a reason to be in Vice City. The modes that could be in any
game -- the derby as "last one running", the hill as "hold the circle" -- are
the ones that would be better if they were more Vice City. The set leans on
rules and treats the toy as the skin, which is backwards. Caddy football gets
it right: the object *is* the joke and the rules are nearly incidental. There
is a mismatch there too -- the football's menu venue is the golf course, which
is perfect, but the in-world gesture builds the pitch wherever the party
stopped, which is a car park.

## Not covered

About 100 `IsPlayer()` sites in `Ped.cpp` / `PedAI.cpp` (grep only), seat-entry logic
(`PedSetInCarCB`, `SeekCar`, hop-in), garages, death/arrest flow, `pedtype.dat` relationships for
PLAYER2-4 (the data isn't in the repo), replay playback of a recorded partner, and anything that
needs hardware: Wii frame cost, the librw async-present and skin-ring changes, whether VRes survives
a remote reconnecting.
