# Couch co-op (GTA:SA PS2 model) — design

Branch: `coop-4` (the four player work, on top of `couch-coop`), itself on top
of `definitive-qol`.

Target model is the **PS2 GTA: San Andreas co-op** the Wii was built alongside:
up to four players, **one shared camera, one screen**, no split-screen. Player 1
stays the script driver; each partner is a real ped who walks, aims, shoots,
drives and rides along, but never owns a mission.

This document describes the design **as it is built**. An earlier version of it
was written before any of the code could be run, and three of the things it
stated as fact turned out not to be; those are listed under "What the first
draft got wrong", because each one cost time and each is the kind of thing that
gets re-believed.

The two-player build has been through hardware testing. The four-player work is
staged: the session, the pads, the arsenal, the wanted level and the peds are
four-player now; the camera and the HUD still speak the pair's shape and are
the next stages (see `COOP-4-PLAN.md`). Four controllers in one room have not
been tried yet.

## The rules, and why each one

**Freeroam only: co-op does not run during missions.** This is a hard boundary,
not a degradation, and it is the thing that makes the rest affordable. Mission
scripts, their cutscene cameras, their fail states and every `PlayerInFocus`
assumption inside a scripted sequence were written for one player; with co-op
off while `CTheScripts::IsPlayerOnAMission()` is true, none of that ever runs
next to another ped. It is also what SA did: there was no co-op campaign.
In practice "off" means the partners are taken out of the world and the camera
is the ordinary one again; all of it comes back when the mission ends.

**`PlayerInFocus` stays 0.** `FindPlayerPed()` is player 1 forever. Scripts, the
HUD, pickups, the wanted level and the save all keep talking to the player they
have always talked to, and nothing has to learn that more exist.

**One camera, and aiming never moves it.** The shared camera takes no input
from the pointer or a stick. Each player's pointer is a reticle — a mark on the
screen — so several people can share one view without fighting over it.

**Each player aims at their own reticle, from their own gun.** Not along the
camera. See "Aiming" below; this is the part the first draft had wrong in a way
that mattered.

**The partners are not saved.** They are `PEDTYPE_PLAYER2..4`, and
`CPools::SavePedPool` picks what to write by `PEDTYPE_PLAYER1`. Every existing
save stays byte-identical and valid.

**Drop-in, drop-out.** With the menu toggle on, a partner appears when their
controller is connected and joins with a button press, and leaves when it has
been gone a few seconds. Nobody has to go back to the menu.

## How it is built

### The session — `CCoop` (`src/core/Coop.cpp`)

One class owns co-op: whether it is running this frame, who the partners are,
and where each player is aiming. `CCoop::Update()` runs once a frame from
`CGame::Process`, after the scripts and before the world.

`CCamera::bWiiCoopCamera` is only the menu toggle — it says the players *want*
co-op. `CCoop::PairActive()` says whether the shared-view half is actually on:
the session is running and at least one partner has joined. The session itself
is also off during missions, cutscenes, replays and while player 1 is wasted or
busted — and until a partner joins, the game is the ordinary one: the follow
camera and its own crosshair, the scopes, the drive-bys. Anything outside the
co-op layer whose behaviour depends on co-op reads `PairActive()` (or
`UsesReticleAim()`, below), never the toggle.

### The partners

- `NUMPLAYERS` is 4. Slots 1..3 of `CWorld::Players[]` are the partners' and
  are nil whenever nobody is playing them. `COMMAND_CREATE_PLAYER` accepts
  slot 0 only.
- Each partner is a `CPlayerPed`, `PEDTYPE_PLAYER2..4` (keeps them out of the
  save), `MISSION_CHAR` (keeps them, and any car they are sitting in, from
  being deleted by the population code — the same protection `CREATE_PLAYER`
  gives player 1), `bStayInCarOnJack` (stays seated when another player gets
  back into the car) and `bDontDragMeOutCar` (a player coming to the passenger
  door climbs past them instead of hauling them out).
- Each slot's `m_pPed` is a registered reference, so the engine nils it if it
  deletes the ped. `CCoop` never caches the pointers; nil means "bring them
  back".
- **Input** is a pad slot of its own per partner, `PAD_COOP`..`PAD_COOP3`
  (indices 2..4). Not pads 1..3: outside `MASTER` builds pad 1 is the engine's
  debug pad — Circle (the fire button) toggles the debug camera, Start hides
  the HUD, and so on.
  `GetPadFromPlayer(ped)` returns the right pad for a player ped; code that
  reads a pad on a player's behalf while processing a ped uses it.
- **Controller**: player 1 is what it always was (a GameCube pad in port 1,
  else Wii Remote 1). The partners are the remaining controllers in order —
  GameCube pads in port order first, then Wii Remotes with a Nunchuk or Classic
  Controller, the same rule player 1 is held to.
- **Joining takes a button press.** A controller being connected is not a
  player: the remote that launched the game is still switched on while its
  owner plays on a GameCube pad, and would otherwise put an idle Tommy beside
  everyone who plays that way. The help box says "Player 3: press any button to
  join" once when that controller is noticed — one line per partner, so it
  names the right player. They leave when their controller has been gone for
  6 s, and one partner leaving is nobody else's business.

### Coming and going

Everything that has to move a partner does it by **replacing** them: the ped
is deleted (the destructor already handles every state a ped can be in) and a
fresh one is put beside player 1, carrying over health, armour, their weapons
and ammo, and which one was in hand. That covers:

- **Dying or being arrested.** Nothing in the engine brings a player back except
  `CGameLogic`, which only knows the player in focus. A partner is left where
  they fell; another player who reaches them and holds for a second brings them
  back **where they fell**, and if nobody does, after 10 s they come back beside
  player 1 as before. Arrested is the one nobody can be brought back from —
  they wait out the 10 s. A partner at their last point of health in a car is
  healed where they sit instead.
- **Getting past the leash** (28 m — see "Staying together"), falling through
  the world, being driven off by an NPC, or player 1 being teleported by a
  script. The world is only streamed around player 1, so a partner left behind
  is on ground that is about to stop existing.
- **No seat.** If player 1 is driving something with no free seat (or a boat),
  the partner waits out of the world and rejoins when player 1 stops or gets out.
- **A change of clothes, a cutscene loading, a replay** — anything that unloads
  the player model or rebuilds the ped pool. `CCoop::Suspend()`.

### Staying together

There is a limit on how far apart the party can get, in two parts. It is a
**star**: every partner is held to player 1, not to each other, and player 1 is
held to each partner the same way.

**On foot it is a wall** (`CCoop::LimitSeparation`, applied to the walking
velocity in `CPed::UpdatePosition` and to a jump's take-off). Past 18 m a
partner moving away from player 1 is slowed, and at 24 m they are stopped;
player 1 is held to each partner the same way, one wall after another, so no
one can leave the others behind. Walking back, or sideways along the edge, is
untouched. It is sized so that at the wall two players are still on screen; with
more than two, the wall is the lever to tighten if the spread ever runs off the
screen (see `COOP-4-PLAN.md`). The help box says so the first time someone
reaches it.

**The wall only works on someone walking**, so behind it there is a leash at
28 m for everything that gets past: a car driving off, a fall, the blast from an
explosion. Past the leash a partner is brought back to player 1 — into the
passenger seat if there is one going, otherwise beside them. A car brought back
is put one notch further behind player 1 for each partner, so several coming
back at once line up along the road instead of piling onto one node.

### Riding along, and driving

- **Getting in with another player.** A partner presses the enter/exit button
  (2 on the Wii Remote, Y on a GameCube pad, X on a Classic Controller) within
  12 m of a car another player is driving or getting into, and there is a free
  passenger seat. If it is standing still they walk to the nearest free
  passenger door and get in the way anyone does; if it is already rolling they
  are put straight into the seat, since nobody catches a moving car on foot.
  On a bike they ride pillion, and are always put straight onto it, and only
  once its rider is sitting on it: the engine hands a bike to any player who
  finishes climbing on, so a partner still walking over when the rider got off
  again would be left on a bike with no rider, which the bike's own code does
  not survive. The seat's vehicle travels with them through the respawn, so a
  moving car or a bike that belongs to partner 2 seats them into *that*
  vehicle, not player 1's. A seat somebody is already walking to is never
  offered to a teleport -- the vehicle flags the door -- so a partner can
  never land in the seat another player is climbing into, and nobody finishes
  their entry with no seat. Scripted passengers -- a taxi fare, a patient --
  walk in through the engine's own enter path, which checks the same seats
  and door flags and gives up cleanly when the car is full: a cab full of
  partners leaves the fare waiting until one gets out, and it retries on its
  own. A partner never drags an NPC out of a seat either: their passenger
  entry turns into a carjack exactly then, and that is cancelled.
- **Taking their own car.** With no free seat beside another player -- player 1
  on foot, in a full car, or in a boat -- the same button takes the nearest car
  or bike within 10 m that another player is not driving and enters it as its
  **driver**. It is the same enter-car path player 1 uses, carjacking an
  occupied car if that is the nearest. Boats are deliberately left alone (see
  "Not done"); a bike is not, because its controls and drive-by come off the
  rider's own pad now. The camera turns with the car the partner is driving when
  player 1 is not in one, so it can be driven rather than felt for. This is
  what a coupe with a full party comes to: its one seat carries one partner
  and the rest drive their own cars, which the regroup then keeps in convoy.
- **Being left behind.** If player 1 simply drives off, the leash does the same
  thing at 28 m: the partner appears in a free seat.
- **No free seat** (a full car, a boat, a train): the partner waits out of the
  world and reappears beside player 1 when they stop or get out.
- **Riding and driving.** As a passenger the partner sits, does not steer, and
  shoots out of their own window (see the drive-by below). As a driver the
  partner steers, accelerates and brakes from their own pad, and their drive-by
  works; they still stay put while player 1 gets out and back in.
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
  seconds later. If an NPC takes the wheel, the partner is brought back to
  player 1; a car any player is driving is part of the party and the leash
  keeps it near. Player 1's own enter button treats a car a partner is driving
  as a passenger seat rather than a carjack, and a partner's button never picks
  a car another player is driving, so nobody can take anybody else's wheel.

A boat is the one vehicle a partner is only ever a passenger in: the engine
makes any player who boards one its driver whatever seat they asked for, so
rather than hand the wheel over by surprise, no partner is ever offered one
(see "Not done"). A partner is never allowed to finish dragging someone out of
a seat either (`CPed::PedSetInCarCB` makes whoever did the dragging the
driver).

A **drive-by** under the shared camera works for whoever is driving: the side is
taken from the look-left/look-right buttons directly, the way the engine already
does it for its own top-down and cinematic cameras, instead of from which way
the car camera has swung. With a reticle up -- either player's -- the side comes
from which side of the car it is on instead, and the shot itself goes where the
reticle is, within the cone the single-player pointer correction uses; outside
it the stock window direction stays, because nobody shoots across their own car.

A player in a **passenger seat** shoots too.  Their side is their seat's -- a
rear-left passenger has the left window and no other -- and their shot follows
their reticle when it is on that side, on the same terms.  The traffic's
passengers still do not fight.  A bike's pillion and a boat's passenger do not
shoot: their vehicles' drive-bys are the driver's alone.

A Rhino's turret and a fire truck's hose are turned with the pad, as in stock
Vice City, and each belongs to whoever is driving: `TankControl` and
`FireTruckControl` take the driver from the vehicle, with their own pad, so a
partner who steals one drives and fires it. A Wiimote and Nunchuk have no right
stick, so on that controller the Nunchuk's lean stands in for it in a heli and
a tank (see the input layer). AIM IN CAR otherwise hands them to the car camera
that follows the pointer, which does not run under the shared one, so
`CCamera::UseFreeCarCam` answers no while co-op is running.

### Weapons

Pickups, shops and scripts all hand weapons to `FindPlayerPed()`, and that is
left alone. Instead every partner is given the same **weapons** player 1 has —
one pickup arms the party — but they all draw from **one shared pool of ammo**,
not a copy each. Player 1's `m_nAmmoTotal` is the pool and each partner's is
kept equal to it, so a burst from one empties the others too.

- **The clip and the reload stay each player's own**, so a reload still takes
  its own time and a magazine is not shared. A partner's clip is clamped to
  what the pool has left, so it can never hold rounds another player has
  already spent.
- Each partner's own `m_nAmmoTotal` is only a **cache of the pool**, re-synced
  every frame (`CCoop::SyncSharedAmmo`), with what every partner spent summed
  into one subtraction. This is the whole of the ammo race: everyone reads and
  writes one count, so several players firing the same gun cannot fire the same
  round twice. The pool drains N−1 times faster with N players; that is
  deliberate, and worth revisiting only if it feels thin on hardware.
- Partners **switch weapons independently** — `ProcessWeaponSwitch` is per-ped
  and reads their own pad — but draw from the pool.
- A partner who is **new, or who died**, gets a copy of every weapon player 1 is
  carrying. A partner who was only **moved** (regrouped, seated, set aside for a
  mission) keeps their own weapons. The ammo behind them is the pool either way.
- **A rampage's weapon is not shared.** `CDarkel` puts it in one of player 1's
  slots and takes it out afterwards; that slot is ignored while it lasts
  (`CDarkel::GetFrenzyWeaponSlot`). The partners fight the rampage with their
  own, and their kills count toward it.
- The **HUD** shows player 1's weapon, which *is* the shared count, so it is
  everyone's when they hold the same gun. Each partner's own readout is the
  ammo line under their pip (see "Marks on screen").

### The camera

`MODE_WII_COOP`, **requested through `CCamera::CamControl` like any other mode**.
It stands in for every camera whose job is simply following the player (on foot,
in a car, the lock-on and scope cameras) and stands aside for everything the game
points on purpose: garages, the arrest and death cameras, trains, anything a
script directs.

- The pitch is fixed on foot and drops 14° while a moving car is being followed,
  so driving looks down the road rather than at the roof. The distance comes in
  when the players are close together, and goes back out as they separate and as
  the car speeds up.
- Fixed heading on foot — whatever way the view was facing when it took over.
  It follows a car player 1 is driving, because a camera pitched down from behind
  sees four times as far ahead as behind and driving toward it is driving blind.
- Looks at the midpoint of player 1 and the partner furthest from them, so the
  view stays on the line between the two it can see least far across. Its
  height follows slowly, so kerbs and steps do not shake the view, but never
  from more than 6 m behind. The full four-player shape — a centroid and the
  widest pair, with the yaw following the vehicle carrying the most players —
  is stage 4; see `COOP-4-PLAN.md`.
- Comes in along its own line when a building is in the way, so indoors it ends
  up under the ceiling rather than looking at the roof. The line that is tested
  runs back from **player 1 and the framed partner**, not from the midpoint:
  the midpoint is an average and can be inside a staircase or under a floor,
  and from in there the test finds the surface it started beneath. It takes the
  roomier of the two answers — it cannot be under one player's ceiling and
  still show the other down the street. Stage 4 extends the test to every
  player.
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
  | 0 low | 36° | 18 m |
  | 1 middle (default) | 45° | 22 m |
  | 2 high | 55° | 26 m |
  | 3 overhead | 73° | 28 m |

  The distances are what the camera stands at six metres apart (the point the
  view starts backing off); closer together it comes in. The pitch has been
  argued both ways on paper more than once and settled neither time; it has to
  be chosen by looking at it. That is what the button is for.

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

- **The players can shoot and punch each other** — it is more fun that way,
  and there is a **Friendly Fire** toggle on the co-op page (on by default) to
  turn it off. With it off, a hit that lands on a player from a player -- or
  from a car one of them is driving -- costs nothing; the glance and the blood
  still show, because the hit feedback is what makes a fight read.
  **Nothing aims at another player for them:** Classic lock-on skips them, the
  aim assist skips them, and a reticle resting on another player does not turn
  red or snap; it is treated as pointing past them.
- **What they do to each other is not a crime** and not a statistic: no wanted
  level for hitting another player, and a dead partner is not a rampage kill.
- **The heat is shared** for everything else, and the **Shared Wanted** toggle
  (on by default) is what "shared" means. With it on, a crime by anyone raises
  everyone to the highest level; drops decay on their own. With it off each
  player keeps their own record — but a partner's rise is still put on player
  1, because every cop in the city reads player 1's wanted level and nobody
  else's; without that a partner's crime would call nobody.
- **What counts as a player's.** One profile: a partner's kills count toward
  the rampage (and the victim blames the actual killer) and into the stats,
  havoc and media attention are the party's, a script-set "only damaged by
  player" yields to any player, a partner shooting a police car raises the
  shared wanted level, and the fast-reload cheat speeds up everyone's reload.
  A vehicle's one-time bonus — the ambulance heal, the taxi fare, the police
  shotgun, the enforcer armour, the caddy club — goes to whichever player
  first drives it: health and armour to them, the weapons and the money to
  the shared arsenal and wallet. The AI and the law still read player 1 alone,
  by design: they chase and target the anchor.
- **Notices.** The help box says when a partner joins or leaves — by name, one
  line per partner — when co-op is paused for a mission, and which framing the
  camera button picked.
- **Marks on screen.** One colour per player (cyan, pink, green, amber), worn
  by their reticle and by a pip with a health bar and an ammo line over their
  head -- one set per player, shown once a partner is in the world. The menu
  and the per-partner skins are stage 5.
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

- **No partner ever takes a boat.** Cars and bikes they can steal and drive
  (see "Riding along, and driving"); a boat is deliberately left alone, so
  nobody finishes climbing on one and gets made its driver by surprise. A
  boat's passenger rides without a drive-by, and so does a bike's pillion:
  their drive-bys are the driver's alone.
- **Boats and trains**: a partner waits out of the world. They can still be a
  passenger in another player's car, pillion on their bike, or riding in a
  boat.
- **The wallet and the arsenal are player 1's.** Everyone collects pickups now,
  but weapons and ammo, money, packages and property are credited to player 1
  -- a partner gets the weapon back through the usual mirror -- so a money
  pickup a partner walks over is really player 1's. Health, armour, adrenaline
  and a bribe stay with whoever walked over them.
- **No remote speaker for the partners.** The speaker code drives one remote,
  and each partner's would need its own stream. Rumble works for everyone
  (gunfire, explosions, the car or bike they are in, thunder).
- **The partners' skins are one choice, not one each.** The co-op page's
  Partner Skin picks from the special-character models and every partner wears
  it; the default is player 1's model, which means player 1's skin texture too
  (`RenderPlayerCB` applies slot 0's). Per-partner skin rows are stage 5.
- **Indoors** the camera ends up close under low ceilings. It works; it is not
  pretty.
- **A camera that tilts up by itself near walls.** The honest fix for tall
  buildings on the camera's side (see the framings). It was left out on
  purpose: it needs a controller with hysteresis, and it has to tell a wall
  (steeper is clearer) from a ceiling (steeper is *worse* — the room under a
  ceiling of height h is h/sin(pitch)), which means probing both ways every
  frame. A camera that nods is worse than one that is sometimes tight, and
  there was no way to watch it. The overhead framing is the manual version.
- **The Classic lock-on's target marker is single.** Under Classic controls
  the lock-on marker is the engine's one, so with several players locked on it
  shows one of them. A moment's oddity, not a fault. (The jack-cancel tap, the
  melee combo flag and the weapon-change lock are per player now.)
- **Sound is quieter.** The listener is the camera, and it is 15–30 m from the
  players instead of 4. (Height hardly counts — the audio code scales it by a
  fifth — but the distance back does.) Footsteps and nearby chatter suffer most.
- **Pop-in at the top of the screen when the players are far apart.** Pedestrians
  are created about 40 m from player 1, on the understanding that this is off
  screen; pulled all the way back, the low framing sees further than that.
- **A partner's ammo readout is the pip.** The HUD proper shows player 1's
  weapon, which is the shared count; each partner's own gun and its count are
  on their pip, and the engine switching weapon when the pool runs dry is the
  only other notice they get.
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
| leash (on foot) | `kLeash`, `Coop.cpp` | 28 m |
| leash (driving) | `kLeashDriving`, `Coop.cpp` | 50 m |
| regroup lane per partner | `kRegroupBack` steps, `Coop.cpp` | 12 m further back each |
| board another player's car within | `kBoardingRange`, `Coop.cpp` | 12 m |
| own steal range | `kStealRange`, `Coop.cpp` | 10 m |
| camera's furthest | `kCoopMaxDistance`, `Cam.cpp` | 40 m |
| revive: reach within / hold for | `kReviveRange` / `kReviveHoldMs`, `Coop.cpp` | 2 m / 1 s |
| downed bleed-out | `kDownedBleedMs`, `Coop.cpp` | 10 s |
| join / drop-out delay | `kJoinAfterMs` / `kDropAfterMs` | 0.4 s (then a button) / 6 s |
| pointer lapse | `kPointerFreshMs` | 1.5 s |
| shot follows the reticle within | `kAimArc`, `Coop.cpp` | 50° of the gun |
| view height may trail by | `kCoopHeightLag`, `Cam.cpp` | 6 m |
| assist cone | `kAssistMinCone` / `kAssistMaxCone` | 2.5° / 8° |
| player colours | `kCoopColours`, `Hud.cpp` | cyan / pink / green / amber |
