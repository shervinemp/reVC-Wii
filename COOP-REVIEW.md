# Co-op review: coop-4-perf (2026-10-18)

Scope: everything the multiplayer work changed since 352ca4b4 (about 8,600 added lines).
Nothing was built or run. Five reviewers read the code, one area each, and I read the menu,
HUD and text changes myself.

**Status (2026-10-18):** items 1, 2, 4, 5 and 6 are fixed in the working tree (uncommitted) and pass a
`-fsyntax-only` compile with the Wii build's flags; item 3 turned out to be wrong and needs no fix.
Nothing has been run on a console.

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
    skins but not Friendly Fire, Shared Wanted, Minigames or the co-op toggle.
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

## Not covered

About 100 `IsPlayer()` sites in `Ped.cpp` / `PedAI.cpp` (grep only), seat-entry logic
(`PedSetInCarCB`, `SeekCar`, hop-in), garages, death/arrest flow, `pedtype.dat` relationships for
PLAYER2-4 (the data isn't in the repo), replay playback of a recorded partner, and anything that
needs hardware: Wii frame cost, the librw async-present and skin-ring changes, whether VRes survives
a remote reconnecting.
