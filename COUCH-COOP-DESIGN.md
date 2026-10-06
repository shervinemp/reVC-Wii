# Couch co-op (GTA:SA PS2 model) — design

Branch: `couch-coop`, on top of `definitive-qol`.

Target model is the **PS2 GTA: San Andreas co-op** the Wii was built alongside:
two players, **one shared camera, one screen**, no split-screen. Player 1 stays
the script driver; player 2 is a real second ped who walks, aims, shoots and
rides along, but never owns a mission.

This document describes the design **as it is built**. An earlier version of it
was written before any of the code could be run, and three of the things it
stated as fact turned out not to be; those are listed under "What the first
draft got wrong", because each one cost time and each is the kind of thing that
gets re-believed.

**Nothing here has been run on hardware yet.** It compiles and links, and every
path was written against the engine code it calls, but the first boot with two
controllers is still ahead. See `HANDOFF-COUCH-COOP.md` for the test plan.

## The rules, and why each one

**Freeroam only: co-op does not run during missions.** This is a hard boundary,
not a degradation, and it is the thing that makes the rest affordable. Mission
scripts, their cutscene cameras, their fail states and every `PlayerInFocus`
assumption inside a scripted sequence were written for one player; with co-op
off while `CTheScripts::IsPlayerOnAMission()` is true, none of that ever runs
next to a second ped. It is also what SA did: there was no co-op campaign.
In practice "off" means the partner is taken out of the world and the camera is
the ordinary one again; both come back when the mission ends.

**`PlayerInFocus` stays 0.** `FindPlayerPed()` is player 1 forever. Scripts, the
HUD, pickups, the wanted level and the save all keep talking to the player they
have always talked to, and nothing has to learn that a second one exists.

**One camera, and aiming never moves it.** The shared camera takes no input
from the pointer or a stick. Each player's pointer is a reticle — a mark on the
screen — so two people can share one view without fighting over it.

**Each player aims at their own reticle, from their own gun.** Not along the
camera. See "Aiming" below; this is the part the first draft had wrong in a way
that mattered.

**Player 2 is not saved.** The partner is `PEDTYPE_PLAYER2`, and
`CPools::SavePedPool` picks what to write by `PEDTYPE_PLAYER1`. Every existing
save stays byte-identical and valid.

**Drop-in, drop-out.** With the menu toggle on, the partner appears when a
second controller is connected and leaves when it has been gone a few seconds.
Nobody has to go back to the menu.

## How it is built

### The session — `CCoop` (`src/core/Coop.cpp`)

One class owns co-op: whether it is running this frame, who the partner is, and
where each player is aiming. `CCoop::Update()` runs once a frame from
`CGame::Process`, after the scripts and before the world.

`CCamera::bWiiCoopCamera` is only the menu toggle — it says the players *want*
co-op. `CCoop::IsRunning()` says whether co-op is in charge right now, and is
also false during missions, cutscenes, replays and while player 1 is wasted or
busted. Anything whose behaviour depends on co-op reads `IsRunning()` (or
`UsesReticleAim()`, below), never the toggle.

### The second player

- `NUMPLAYERS` is 2. Slot 1 of `CWorld::Players[]` is the partner's and is nil
  whenever nobody is playing it. `COMMAND_CREATE_PLAYER` accepts slot 0 only.
- The partner is a `CPlayerPed`, `PEDTYPE_PLAYER2` (keeps it out of the save),
  `MISSION_CHAR` (keeps it, and any car it is sitting in, from being deleted by
  the population code — the same protection `CREATE_PLAYER` gives player 1),
  `bStayInCarOnJack` (stays seated when player 1 gets back into the car) and
  `bDontDragMeOutCar` (player 1 coming to the passenger door climbs past them
  instead of hauling them out).
- `Players[1].m_pPed` is a registered reference, so the engine nils it if it
  deletes the ped. `CCoop` never caches the pointer; nil means "bring them back".
- **Input** is a pad slot of its own, `PAD_COOP` (index 2). Not pad 1: outside
  `MASTER` builds pad 1 is the engine's debug pad — Circle (the fire button)
  toggles the debug camera, Start hides the HUD, and so on.
  `GetPadFromPlayer(ped)` returns the right pad for a player ped; code that reads
  a pad on a player's behalf while processing a ped uses it.
- **Controller**: player 1 is what it always was (a GameCube pad in port 1, else
  Wii Remote 1). The partner is the first other controller present — any further
  GameCube pad, then any Wii Remote with a Nunchuk or Classic Controller.
- **Joining takes a button press.** A controller being connected is not a
  player: the remote that launched the game is still switched on while its owner
  plays on a GameCube pad, and would otherwise put an idle second Tommy beside
  everyone who plays that way. The help box says "Player 2: press any button to
  join" once when a second controller is noticed. They leave when the
  controller has been gone for 6 s.

### Coming and going

Everything that has to move the partner does it by **replacing** them: the ped
is deleted (the destructor already handles every state a ped can be in) and a
fresh one is put beside player 1, carrying over health, armour, their weapons
and ammo, and which one was in hand. That covers:

- **Dying or being arrested.** Nothing in the engine brings a player back except
  `CGameLogic`, which only knows the player in focus. The partner is left where
  they fell for 3.5 s and then returns beside player 1 at full health.
- **Getting past the leash** (28 m — see "Staying together"), falling through
  the world, being driven off by someone else, or player 1 being teleported by a
  script. The world is only streamed around player 1, so a partner left behind
  is on ground that is about to stop existing.
- **No seat.** If player 1 is driving something with no free seat (or a boat),
  the partner waits out of the world and rejoins when player 1 stops or gets out.
- **A change of clothes, a cutscene loading, a replay** — anything that unloads
  the player model or rebuilds the ped pool. `CCoop::Suspend()`.

### Staying together

There is a limit on how far apart the two can get, in two parts.

**On foot it is a wall** (`CCoop::LimitSeparation`, applied to the walking
velocity in `CPed::UpdatePosition` and to a jump's take-off). Past 18 m a player
moving away from the other is slowed, and at 24 m they are stopped; walking
back, or sideways along the edge, is untouched. It holds both players equally —
neither can leave the other behind — and it is sized so that at the wall both
are still on screen. The help box says so the first time someone reaches it.

**The wall only works on someone walking**, so behind it there is a leash at
28 m for everything that gets past: a car driving off, a fall, the blast from an
explosion. Past the leash the partner is brought back to player 1 — into the
passenger seat if player 1 is the one in the car, otherwise beside them.

### Riding along

- **Getting in.** The partner presses the enter/exit button (2 on the Wii
  Remote, Y on a GameCube pad, X on a Classic Controller) within 12 m of the car
  player 1 is driving or getting into. If it is standing still they walk to the
  nearest free passenger door and get in the way anyone does; if it is already
  rolling they are put straight into the seat, since nobody catches a moving car
  on foot. On a bike they ride pillion, and are always put straight onto it,
  and only once player 1 is sitting on it: the engine hands a bike to any
  player who finishes climbing on, so a partner still walking over when
  player 1 got off again would be left on a bike with no rider, which the
  bike's own code does not survive.
- **Being left behind.** If player 1 simply drives off, the leash does the same
  thing at 28 m: the partner appears in a free seat.
- **No free seat** (a full car, a boat, a train): the partner waits out of the
  world and reappears beside player 1 when they stop or get out.
- **Riding.** The partner sits. They do not steer and cannot yet shoot from the
  car. They stay put while player 1 gets out and back in.
- **Getting out.** The same button: stepping out once the car has stopped, or
  rolling out of it at speed. The order is only given when the engine would
  obey it (`CanPedExitCar`, or the roll-out test) — one it refuses is kept,
  retried five seconds later and marks the ped a hostage meanwhile, so an early
  press used to lock the door and throw the partner out later. In between
  (too fast to step out, too slow to roll) holding the button gets them out the
  moment it can. Off the back of a moving bike there is no getting out; the
  engine has no such move. If player 1 gets out and walks off, the partner is
  pulled out to their side at the leash.
- **When it goes wrong.** A car on fire puts the partner out of it by itself. One
  killed in the car, or still in it when it explodes, comes back the usual few
  seconds later. If someone else takes the wheel, the partner is brought back to
  player 1.

Only ever player 1's car, and only ever as a passenger: the engine makes any
player who boards a bike or a boat its driver whatever seat they asked for. For
the same reason the partner is never allowed to finish dragging someone out of a
seat (`CPed::PedSetInCarCB` makes whoever did the dragging the driver).

The partner never drives. `CAutomobile::ProcessControl` and friends read pad 0
throughout; that is a separate, larger job.

Player 1's **drive-by** still works under the shared camera: the side is taken
from the look-left/look-right buttons directly, the way the engine already does
it for its own top-down and cinematic cameras, instead of from which way the car
camera has swung.

### Weapons

Pickups, shops and scripts all hand weapons to `FindPlayerPed()`, and that is
left alone. Instead the partner is given whatever player 1 **gains**: a weapon
player 1 did not have a moment ago, or the ammo player 1's count just went up by.
One pickup arms both players; the partner's ammo is still their own to spend.

- A partner who is **new, or who died**, gets a copy of everything player 1 is
  carrying.
- A partner who was only **moved** (regrouped, seated, set aside for a mission)
  keeps their own weapons and ammo, plus whatever player 1 gained while they
  were away.
- **Swapping a weapon for another in the same slot** (M4 for a Ruger) passes on
  only the difference, because that slot's ammo carries over from one weapon to
  the next for the partner just as it did for player 1.
- **A rampage's weapon is not shared.** `CDarkel` puts it in one of player 1's
  slots and takes it out afterwards; that slot is ignored while it lasts
  (`CDarkel::GetFrenzyWeaponSlot`). The partner fights the rampage with their
  own.

### The camera

`MODE_WII_COOP`, **requested through `CCamera::CamControl` like any other mode**.
It stands in for every camera whose job is simply following the player (on foot,
in a car, the lock-on and scope cameras) and stands aside for everything the game
points on purpose: garages, the arrest and death cameras, trains, anything a
script directs.

- Fixed pitch. Only the distance changes: further back as the players separate
  and as the car speeds up.
- Fixed heading on foot — whatever way the view was facing when it took over.
  It follows a car player 1 is driving, because a camera pitched down from behind
  sees four times as far ahead as behind and driving toward it is driving blind.
- Looks at the midpoint of the two players. Its height follows slowly, so
  kerbs and steps do not shake the view, but never from more than 6 m behind.
- Comes in along its own line when a building is in the way, so indoors it ends
  up under the ceiling rather than looking at the roof. The line that is tested
  runs back from **each player**, not from the midpoint: the midpoint is an
  average and can be inside a staircase or under a floor, and from in there the
  test finds the surface it started beneath. With two answers the camera takes
  the roomier one — it cannot be under one player's ceiling and still show the
  other down the street.
- The **cinematic camera** and Classic controls' **look-around** are off while
  co-op runs. Both take the view (and the second also the controls) away from
  the players, and co-op has the button that would give them back.
- **The renderer had to be told about it.** Several things in the engine assume
  the camera stands right behind the player, and none of them are in `Cam.cpp`:
  - *Shadows* are only drawn within 13–27 m of the camera, which from this
    camera's position is nobody on screen. They are measured from the point the
    camera looks at instead (`ShadowViewPoint`, `Shadows.cpp`).
  - *What gets drawn at all* is whatever is in the map sectors under a triangle
    from the camera to the far top corners of the view. A camera looking steeply
    down also sees a wide strip beside and beneath itself, which that triangle
    misses; two wedges are added to cover it (`ScanCoopViewEdges`,
    `Renderer.cpp`). Without them, buildings in the bottom corners of the screen
    come and go with the sector grid.
  - *Heat haze* is laid across the screen where a level camera has its horizon;
    it is off, as it is for the engine's own top-down modes.
  - *Drive-bys* read the side to shoot from off the car camera; see "Riding
    along".
- **Four framings**, stepped through in game with player 1's camera button
  (− on the Wii Remote) and kept in the INI as `CoopFraming`:

  | | pitch | distance |
  |---|---|---|
  | 0 low | 41° | 18 m |
  | 1 middle (default) | 50° | 22 m |
  | 2 high | 60° | 26 m |
  | 3 overhead | 78° | 28 m |

  The pitch has been argued both ways on paper more than once and settled
  neither time; it has to be chosen by looking at it. That is what the button is
  for.

  The fourth is there for a different reason. The camera does not turn on
  foot, so a tall building on its side of the street is between it and anyone
  on that pavement, and the only thing it can do is come in close — within
  about 14 m of such a building the view tightens, and right against it the
  camera is pinned at 3 m. From overhead there is no such side. A camera that
  tilted up by itself near walls would be better still; see "Not done".

### Aiming

`CCoop::UsesReticleAim()` — co-op running, Standard controls, and the shared
camera actually on screen. Classic controls keep their lock-on, which never
needed the camera.

Each frame, per player:

1. **Where is the reticle?** A Wii Remote pointer if that player has one;
   otherwise the right stick as a direction (twin-stick style), with the reticle
   drawn 9 m out along it. A pointer that goes quiet for 1.5 s lapses.
2. **What is under it?** The ray through the reticle is followed into the world.
   On a ped or a vehicle, that exact point is the target. On scenery, the player
   means a *direction*, and the ray is met with a level plane at the player's own
   chest height — not the ground, which seen from above and behind is a metre
   past a target's feet and enough to miss by from the side.
3. **Assist.** While firing, a reticle that is on nothing still bends the shot
   to a live target that is nearly in line (the same cone `CAimAssist` uses, and
   the same "Aim Assist" option switches it off). On-screen targets only. The
   reticle turns red on a target.

**An aim does not lapse while it is being used.** While a player is firing, a
direction that has stopped arriving — the right stick let go so the same thumb
can hold the trigger, the pointer off the screen — is held where it was, and a
player who never gave one is taken to be aiming the way they face. So anyone
holding fire strafes facing a fixed direction, whatever they aim with. Without
this the aim ran out mid-burst and the controls changed under the player's
hands to ones that do not turn or walk while firing.

Shots then leave **the gun**, toward that target — `CCoop::FindShotVector`,
called from `CWeapon::FireInstantHit`, `FireShotgun`, `FireAreaEffect` and
`FireProjectile`. With only a direction, the line is level and the engine's own
vertical auto-aim (`DoDoomAiming`) finishes it. **The shot goes to the reticle
only when the gun is pointing within 50° of it**, and straight out of the gun
otherwise: the body turns toward the reticle at its own pace and a sprinting
player is not facing it at all, so a round sent to the reticle regardless would
leave sideways or backwards through the shooter. The shooter's own collision is
ignored by their shots for the same reason.

There is **no scope**: the scope is a camera and there is one camera. The two
sniper rifles fire as long-range instant-hit weapons and the rocket launcher
fires along the aim — one shot per press, straight from the weapon with no
animation, which is how the scope itself fires them. They cannot use the
ordinary attack: `weapon.dat` gives all three a bare fist's animations, because
the stock game never shows a player firing one. The camera (the weapon) does
nothing.

### Which way a player faces

`CCoop::FacesAim`, decided once a frame per player:

- **Gun (or anything thrown) out, and an aim** → the body faces the reticle, and
  the stick moves the player across the screen whichever way they are facing.
  This is the stock "first-person run-around" control with its strafing and
  backing-up animations; the only change is that the stick is read against the
  screen rather than against the body.
- **Sprint held** → the body faces the way the stick points (the Classic
  control). Facing the reticle while running somewhere means running sideways at
  the pace those animations move. Shots fired like this go where the gun
  points.
- **Fists, a bat or a blade** → the Classic control too, **including through
  the swing**. The engine's own fight picks who is hit and turns the body onto
  them for each blow; pulling the body back to the reticle between blows made
  every one of them miss. Melee is not aimed. (Crouched is the one exception,
  because the Classic control does not let a crouching player move.)
- **Gun out but no aim** (no pointer, stick untouched, not firing) → the
  Classic control, until the trigger is pulled.

### Smaller rules

- **The players can shoot and punch each other** — it is more fun that way —
  **but nothing aims at the other player for them.** Classic lock-on skips
  them, the aim assist skips them, and a reticle resting on the other player
  does not turn red or snap; it is treated as pointing past them.
- **What they do to each other is not a crime** and not a statistic: no wanted
  level for hitting your partner, and a dead partner is not a rampage kill.
- **The heat is shared** for everything else. There is one wanted level,
  player 1's, and the partner's crimes count toward it.
- **Notices.** The help box says when player 2 joins or leaves, when co-op is
  paused for a mission, and which framing the camera button picked.
- **Marks on screen.** One colour per player (cyan, pink), worn by their reticle
  and by a pip with a health bar over their head. The pips only appear once there
  are two players; the health bar is the only place the partner's health is shown.
- **The stock zoom does not cycle** while co-op has the camera button.

## What the first draft got wrong

Recorded because each was stated as a verified fact.

1. **"The port has exactly one input sink" — it does not.** `WiiPadState.cpp`,
   with its `lwjgl::Keyboard` and `lwjgl::Mouse`, is a leftover from a different
   port. It is behind `#ifdef WII_PLATFORM`, which nothing defines, and it is not
   in the build. Buttons and sticks were already per-pad in `WiiPad.cpp`; what
   was actually shared was the pointer (one crosshair on `CCamera`) and a handful
   of pad-0 reads in ped code.
2. **"Gate 0 — the shared camera — done" — it had never run, and could not have
   worked.** It forced its mode from inside `CCam::Process`, underneath the mode
   selection in `CCamera::CamControl`, so every frame the selector saw the wrong
   mode and started a transition back: one frame of shared view, a second and a
   half of follow camera, repeat. It also took its heading from player 1's
   facing while the control code took player 1's facing from the camera, which
   spins; eased with `GetTimeStep()` (fiftieths of a second) where it meant
   seconds; scaled only its distance back and not up, so the pitch flattened as
   the players separated; and left `DirectionWasLooking` and
   `m_cvecTargetCoorsForFudgeInter` stale. All of that is replaced.
3. **"The shot ray goes through the reticle, so aiming stays correct" — only for
   a camera over the shoulder.** Along a camera ray from overhead, a shot starts
   in mid air and hits whatever can be seen from up there, walls or no. Hence
   shots from the gun.
4. The fix in `CPed::FinishLaunchCB` for a second player's jump was written into
   the `#else` of `#ifdef FREE_CAM`. `FREE_CAM` is defined; that branch is never
   compiled.

## Not done

- **Player 2 cannot drive**, and cannot do drive-bys as a passenger. Vehicle
  control reads pad 0 throughout.
- **Boats and trains**: the partner waits out of the world.
- **Pickups** are still player 1's (the partner gets weapons by mirroring).
  Health, armour and money pickups do nothing for the partner.
- **No rumble and no remote speaker for player 2.** The engine sends every shake
  to pad 0, and the speaker code drives one remote.
- **Player 2 looks exactly like player 1.** The pips are what tells them apart.
  A different model needs a model slot scripts will not reuse.
- **Indoors** the camera ends up close under low ceilings. It works; it is not
  pretty.
- **A camera that tilts up by itself near walls.** The honest fix for tall
  buildings on the camera's side (see the framings). It was left out on
  purpose: it needs a controller with hysteresis, and it has to tell a wall
  (steeper is clearer) from a ceiling (steeper is *worse* — the room under a
  ceiling of height h is h/sin(pitch)), which means probing both ways every
  frame. A camera that nods is worse than one that is sometimes tight, and
  there was no way to watch it. The overhead framing is the manual version.
- **A few statics are still shared between the players**: the jack-cancel tap
  (`cancelJack` in `CPed::ProcessControl`), the melee combo flag
  (`nPlayerInComboMove`), and under Classic controls the lock-on's
  `bDontAllowWeaponChange` and its single target marker. Each is a moment's
  oddity, not a fault.
- **Sound is quieter.** The listener is the camera, and it is 15–30 m from the
  players instead of 4. (Height hardly counts — the audio code scales it by a
  fifth — but the distance back does.) Footsteps and nearby chatter suffer most.
- **Pop-in at the top of the screen when the players are far apart.** Pedestrians
  are created about 40 m from player 1, on the understanding that this is off
  screen; pulled all the way back, the low framing sees further than that.
- **Ammo for player 2 is not shown anywhere.** The engine switches weapon when
  one runs dry, which is the only notice they get.
- **Dynamic control ownership** — SA let the players hand "control" back and
  forth, with the camera following whoever had it. Here the camera frames the
  midpoint and player 1 is the anchor. Framing a player's lock target is not
  done either.
- **Co-op missions.** Custom `.scm` missions that opt in remain possible later.

## Tunables

| what | where | value |
|---|---|---|
| framings | `kCoopFramings`, `Cam.cpp` | see table above |
| pull-back per metre apart | `kCoopSeparationGain` | 1.0, beyond 6 m |
| the wall on foot | `kTetherStart` / `kTetherMax`, `Coop.cpp` | slows from 18 m, stops at 24 m |
| leash | `kLeash`, `Coop.cpp` | 28 m |
| camera's furthest | `kCoopMaxDistance`, `Cam.cpp` | 40 m |
| respawn delay | `kRespawnAfterMs` | 3.5 s |
| join / drop-out delay | `kJoinAfterMs` / `kDropAfterMs` | 0.4 s (then a button) / 6 s |
| pointer lapse | `kPointerFreshMs` | 1.5 s |
| shot follows the reticle within | `kAimArc`, `Coop.cpp` | 50° of the gun |
| view height may trail by | `kCoopHeightLag`, `Cam.cpp` | 6 m |
| assist cone | `kAssistMinCone` / `kAssistMaxCone` | 2.5° / 8° |
| player colours | `kCoopColours`, `Hud.cpp` | cyan / pink |
