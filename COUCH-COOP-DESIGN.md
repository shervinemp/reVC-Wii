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
the next stages (see "Not done"). Four controllers in one room have not been
tried yet.

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
  debug pad - Circle (the fire button) toggles the debug camera, Start hides
  the HUD, and so on. While a pair is active the HUD toggle is off and the HUD
  is forced back on: pad 1 is then player 2, and Start must not turn off the
  radar, the help box and a minigame's objective.
  `GetPadFromPlayer(ped)` returns the right pad for a player ped; code that
  reads a pad on a player's behalf while processing a ped uses it.
- **Controller**: player 1 is the GameCube pad in port 1, else the first
  playable Wii Remote to connect in this session - not whichever remote
  happens to be on WPAD channel 0 from the last play. The partners are the
  remaining controllers in arrival order: GameCube pads in port order first,
  then Wii Remotes with a Nunchuk or Classic Controller, the same rule player 1
  is held to. A controller keeps its seat while it stays connected; when every
  remote has gone, the next one to arrive is player 1 again. Remotes already on
  before the game starts tie-break by WPAD channel (the platform reports no
  connection time); a remote connected after that follows them. A remote that
  reconnects on a different WPAD channel is a new arrival: libogc exposes no
  stable remote identity to follow it with.
- **Joining takes a button press.** A controller being connected is not a
  player: the remote that launched the game is still switched on while its
  owner plays on a GameCube pad, and would otherwise put an idle Tommy beside
  everyone who plays that way. A fresh controller joins on A; a brief disconnect keeps the seat; the help box says "Player 3: press A to join"
  when that controller is noticed — one line per partner, so it names the right
  player — and again, at most every 20 s per controller, when someone presses
  +, − or the stick on it, none of which joins. A Wii Remote with nothing
  plugged in is told once that it needs a Nunchuk or Classic Controller. A
  joined controller buzzes for a moment. They leave when their controller has
  been gone for 6 s (the HUD shows a "?" on their badge after 1.5 s, and a
  remote with low batteries is named), and one partner leaving is nobody else's
  business. Only player 1's pad and joined pads open the pause menu or move
  about in it: a remote left on the table cannot pause the game for everyone.

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
- **The payphone is not one of these.** The letterbox a phone call puts up is
  the camera's, not a cutscene's, so a partner standing at the payphone while
  player 1 talks on it stays in the world. The train's letterbox is a set
  piece, and that one does stand the party down — and says so, because a
  partner who vanishes without a word is worse than one who is told.

### Staying together

There is a limit on how far apart the party can get, in two parts. It is a
**star**: every partner is held to player 1, and player 1 is held to each partner
the same way. Two partners are held to each other as well, but by a wider band
(26 m to 34 m, against the lead's 18 m to 24 m) — the camera is the other half
of this rule, and it can only zoom out so far.

**On foot it is a wall** (`CCoop::LimitSeparation`, applied to the walking
velocity in `CPed::UpdatePosition` and to a jump's take-off). Past 18 m a
partner moving away from player 1 is slowed, and at 24 m they are stopped;
player 1 is held to each partner the same way, one wall after another, so no
one can leave the others behind. Walking back, or sideways along the edge, is
untouched. It is sized so that at the wall two players are still on screen; with
more than two, the wall is the lever to tighten if the spread ever runs off the
screen (see "Not done"). The help box says so the first time someone reaches
it.

The wider partner-to-partner band is there because the wall is a star and the
camera is not: it centres on the middle of the party and zooms out to 40 m and
no further, so a star could put two partners 48 m apart with player 1 between
them, and one of them went off the screen with no reticle and nothing to say
so. The band still leaves room to spread out.

**The wall only works on someone walking**, so behind it there is a leash at
28 m for everything that gets past: a car driving off, a fall, the blast from an
explosion. Anyone in a car a player is driving, driver or passenger, gets the 50 m
driving leash instead. A partner who just stands there while player 1 pushes
against the wall for about two and a half seconds is brought back too, so
nobody can be pinned for good. Past the leash a partner is brought back to player 1 — into the
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
  occupied car if that is the nearest. A boat is offered the same way, as an
  unattended one only, and so is a bike, because their controls and drive-by
  come off the driver's own pad now. The camera turns with the car the partner is driving when
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
- **When it goes wrong.** A burning, bombed or exploding car treats a partner
  exactly as it treats player 1: nobody is pulled out or healed automatically.
  They can leave with their own exit button, and if they stay they burn, are
  killed, or go down like anyone else. A partner killed in a car is revived
  beside it when it is safe, or well clear of it when it is burning or wrecked,
  with player 1 as the fallback; King of the Hill's forced revive uses the same
  spot instead of leaving them down. If an
  NPC takes the wheel, the partner is brought back to player 1; a car any
  player is driving is part of the party and the leash keeps it near. Player 1's own enter button treats a car a partner is driving
  as a passenger seat rather than a carjack, and a partner's button never picks
  a car another player is driving, so nobody can take anybody else's wheel.

A boat is the one vehicle nobody rides along in: the engine makes any player who
boards one its driver whatever seat they asked for, and the game gives every
boat a single passenger seat at most (none of the boat models has a door frame),
drawn on top of the driver's, so a boat is never offered as a seat. A partner
drives a boat of their own, or one the smuggling run makes for them (see
"Minigames"). A partner is never allowed to finish dragging someone out of
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
  While a car is being followed — by default the one carrying the most players,
  so a party travelling together turns as one; any partner's camera button
  overrides it with their own car while they drive it, and pressing it again
  hands the view back — it turns with that car's nose, because a camera pitched
  down from behind sees four times as far ahead as behind and driving toward it
  is driving blind.
- Looks at the centroid of everyone in the world, and takes the party's widest
  pair as its span, so the view sits in the middle of the party and what has to
  fit is whichever two are furthest apart. With two players both reduce to the
  old pair framing exactly. Its height follows slowly, so kerbs and steps do
  not shake the view, but never from more than 6 m behind.
- Comes in along its own line when a building is in the way, so indoors it ends
  up under the ceiling rather than looking at the roof. The line that is tested
  runs back from **the players themselves**, not from the centroid: the
  centroid is an average and can be inside a staircase or under a floor, and
  from in there the test finds the surface it started beneath. It takes the
  roomiest of the per-player answers — it cannot be under one player's ceiling
  and still show the others — and a player standing on top of player 1 is
  pruned as the same answer.
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
   reticle stays its player's colour on a target and is drawn twice, the second
   half as big again: the single-player one goes red there, but red swallows
   the pink one and tells nothing to anyone who cannot tell it from green.

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
- **The heat is shared** for everything else, always; there is no option for
  it. The wanted level is the party's, and the level the cops read is player 1's
  (every cop in the city reads player 1's and nobody else's). A crime by anyone
  raises everyone to the highest level: most crimes are registered on player 1's
  record to begin with, and the rise carries across the few that land on a
  partner's own, such as a cop seeing one of them point a gun. A drop is
  followed only on player 1, the level the police read: stars above one do not
  wear off on their own in this game, so without that a Pay'n'Spray would leave
  the partners wanted and the next small crime would put everyone back. When
  player 1 falls, every partner above that level comes down to it; a partner
  already lower is left alone, and nobody reads theirs. A partner who joins, or
  comes back, starts at player 1's level.
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
- **Notices.** The help box says when a partner joins, goes down or leaves — by
  name, one line per partner — when one is brought back to the group or has to
  wait for a seat or out a boat ride (at most once per player per 10 s), when
  the wall is slowing someone (again every 30 s while they lean on it), when
  co-op is paused for a mission (again when a partner touches a pad, at most
  every 20 s, and "back" when it ends), and which framing the camera button
  picked. The first partner to arrive is also told that ammo and money are the
  party's, and which button is the camera for their kind of controller. Only the
  camera lines are the quick 1.5 s kind; the rest stay up for the box's
  ordinary time (3 s and 0.05 s a character). There is one help box, so notices
  wait their turn in a queue of eight behind whatever it is saying (the game's
  own hints included), and one is dropped as stale if it waits 15 s (8 s for a
  "down" line, 60 s for the explanations).
- **Marks on screen.** One colour per player (cyan, pink, green, amber), worn
  by their reticle and by a badge with their number on it, over their head (or
  their vehicle), with a health bar and an ammo line under it -- one set per
  player, shown once a partner is in the world. In a shared car the badges
  follow the seats, not the player slots: the driver is on the left and the
  front passenger on the right, whichever of them is player 1. The colours
  differ in lightness
  as well as hue, and the number is what tells them apart for anyone hue does
  not. A downed partner's bar becomes the seconds they have left to bleed out,
  with "DOWN" under it and a white bar filling while someone revives them
  ("BUSTED", with no revive bar, if they were arrested). A controller that has
  stopped answering gets a white "?" beside the badge. The menu
  and the per-partner skins went to N with stage 5: three skin rows, one each.
- **The stock zoom does not cycle** while co-op has the camera button.

### Minigames

Eight engine-side modes the party starts from the world, behind the co-op
page's MINIGAMES switch (on by default; with it off none arms, and a running
one finishes). One runs at a time. They live in `CoopModes.cpp`, and each is a
thin state machine over the script's own machinery -- spawn recipes, locked
mission cars, `CleanUpThisPed`/`CleanUpThisVehicle` hand-backs -- with no new
assets:

**Where they happen.** A mode started from the menu goes to a named venue, and
the ready-up card names it before anybody has moved: the airport apron (the
derby and the cop shift), the golf course (the Caddy), the stadium car park
(the toy race), the docks (the hill), Ocean Beach (the dance-off and cops and
robbers), the dockyard (the smuggling run). The airport and the beach are
known spots. The others are *bases*: the game walks out from the base in rings
and takes the first square that is dry, flat and clear of the size the mode
needs, and the airport is the fallback for anything it cannot find. A venue
that fails to find open ground is the airport, not a bad one -- the party is
never dropped into traffic again.

- the **cop shift** (siren on in a crewed law car: cases, takedowns, a streak;
  while a case is open the takedown is not a crime),
- the **smuggling run** (player 1 at the wheel of a boat and the horn: the
  rest of the party are given boats of their own beside it; a crate each leg,
  first to it carries it to the drop while the other covers; three coastal
  drops, stars and Predators for heat, an Uzi for the last drop),
- the **Bloodring derby** (park a knot of cars and honk: last car running,
  first to two rounds, weapons off),
- the **RC race** (jump together on foot: the racers are seated directly in
  their toys, four gates in order, three laps, each driver's next gate in
  their colour; the party leash stands aside so a toy can take the far side
  of the ring without being pulled back),
- the **dance-off** (everyone ducks together: prompts on a climbing beat; the
  dancing is the players' own bodies), and
- **cops & robbers** (the nod held for 0.6 s while gathered: grab the case,
  deliver or bust, first to three),
- **Caddy football** (everyone in their own car, one player holds the brake: a
  driverless Caddy is the ball; two goals for 1v1 or 2v2, three goals in a
  triangle for three players; first to three; not while wanted), and
- **King of the Hill** (everyone holds aim on foot: a 12 m ring on the nearest
  clear ground, never the roadway; the hill is a *point* at its centre and the
  player nearest it holds it; the hold belongs to the hill and is lost when the
  hill changes hands or is left empty, so the round is decided by who is
  holding the point when the bell goes; player-on-player damage is forced
  on; 90 seconds; a downed or arrested partner is brought back
  at the body after four seconds, and player 1 comes back on the same clock
  without the hospital reset). A player who stays in a car is a spectator, not
  a second scoring position, and a player in a car cannot make the ring
  contested. The wall and leash stand aside for the round, because two players
  at opposite edges of a 12 m ring are at the walking tether's limit already.

  The hill was a ring with a timer on it, and that had two faults. Two people
  standing in the ring made it a deadlock — nobody scored — so the only way to
  play it was to sit on the point and let the other one come, and the other's
  best move was to stand in the ring and refuse to fight, which cost the
  holder nothing and won the challenger nothing. And the score was the *total*
  time held, which is a score the first player to arrive cannot lose: the
  challenger's only way in was the four seconds a knocked-down player spends on
  the ground, which is not a way to win ninety seconds. So the hill is a point
  you have to take, and the hold is lost when you lose it.

The toy race seats its racers directly (`WarpPedIntoCar`) instead of asking
them to walk into a toy: the RC Bandit is a remote-control model with no door
frames, and the ordinary enter-car path is built for cars that have them. A
wrecked toy is replaced and its racer seated again the same way; a racer who is
down or arrested is left where they are.

The toy race, the dance-off, cops & robbers, Caddy football, Bloodring and King
of the Hill start from a jump, a duck, a press of the nod, a horn or a brake,
which a party with the police after it does anyway, so none of the six arms
while anyone in the party is wanted. The cop shift is held back too — the
law's view of the party is that nobody starts a shift with a record — but the
smuggling run is not: it is about the police, and it brings its own heat.

And while a mode is running the party's crimes are not reported to the police
at all. The event system is what the games themselves generate — a car through
a pedestrian in the middle of a derby, a shot in a fight — and it is what
brought the police to a round that was meant to be a race. A script's own
crimes, an alarm or a bank robbery, are not the event system's and still
count. The cop shift is the exception, because there the police are the
point: the takedown is the job, and anything else the party does there is a
crime like anywhere else.

The co-op page also has **START A MINIGAME**, a page of the eight modes for
players who do not want to remember the in-world gesture. Choosing a row arms
the mode; once the menu closes, `CCoopModes::MenuStart` takes over and
`UpdateMenuPending` teleports the party to a clear venue -- the airport apron
for the derby, Caddy football, King of the Hill and the toy race; the beaches
for the dance-off and cops & robbers; a road node for the cop shift; and a
water node off the docks for the smuggling run -- after loading the
destination's collision. If a mode needs a vehicle, the menu makes one:
Bloodring loaners for the derby and Caddy football, a police cruiser (and a
second if the first fills) for the cop shift, and a Squalo with one escort per
partner for the run. A player already in an eligible car keeps it; anyone else
is put on foot at the venue, given the loaner, and seated. The mode's normal
update then starts it from that prepared state, so the gestures still work in
the world and the menu is only another way in.

**What a mode pays, and what it is worth.** The pot scales with the party:
two players get the base, three get one and a half times it, four get twice
it. A pot that does not care how many people are playing makes a four-player
game worth less for each of them than a two-player one, and a party picks the
game with the most people in it. The money goes into the party's one wallet,
which is right for the game's economy and useless as a prize -- so the prize
is a **count**: how many modes each player has won, theirs alone, shown on the
ready-up card and on the result line. It lasts the session, so a loaded game
starts a new one.

The six modes with a winner are the matches. The cop shift and the smuggling
run are **jobs**: they have no winner, they pay per case and per drop, and
they win no trophy.

**Cops & robbers, after a bust.** The case lies dead for three seconds:
nobody can pick it up and nobody can be pinned. It used to be two rules --
the busted player could not take the case back for two seconds, and whoever
took it next could not be busted for two seconds -- and two rules to remember
in the middle of a chase is one rule too many.

**Caddy football, a ball that stops.** A ball that has not moved for six
seconds is wedged -- under a car, pinned in a corner -- and the ramming-only
rules give a wedged ball nobody a way to free it. It goes back to the middle.

The first time a mode arms in a session, one line under the headline says how it
is played and how it ends, for 4.5 s (`DrawExplainer`); it comes back after a
mission or a cutscene, not for a rematch. The derby cuts the fire button for
every player, not only those in it, so that nobody shoots a driver mid-match;
its headline says "ramming only" each round. Every lane has its player's name,
and its bar is cut into one cell for each round, point or lap. The cop shift
shows its streak and the seconds left on the open case, then what the case came
to (caught with the pay, got away, siren off). In cops & robbers the nod is held
(a tap, or a press with a car in reach, which is somebody getting in, starts
nothing); a bust keeps the busted player off the case for two seconds, so the
buster does not score again at once, and whoever picks the case up cannot be
busted for two seconds ("SAFE" on the headline), so a pair glued together
cannot trade busts at the speed of the hold; the screen says who busted whom.
The dance-off's beat runs from 1000 ms to 650 ms, and each chip shows the
action as well as the button.

The smuggling run is player 1's: they are the skipper, because the world is
streamed round player 1 and the whole party is held to them. A boat cannot carry
the party -- no boat model has door frames, so the game gives every boat one
passenger seat at most, drawn on top of the driver's, and a boat passenger has
no button to get off -- so the rest of the party are each given a boat of their
own. When a partner comes into the world with the run on and player 1 at sea,
the run makes a boat of the lead's own model on the water to the side of it and a
little astern, and the partner is put at its wheel (the way a script warps the
player into a car); it is a mission boat, locked, and the save leaves it out. A
partner who is already driving a boat of their own keeps it, and that boat is
the escort: the run treats either the same. The
partners then drive from their own pads; the party leash stands aside so an
escort can race for the crate. The crew is the lead boat and the boats a player
is driving within 80 m of it; the headline says how many ("DROP 2/3  CREW 4") and
each boat of the crew adds $50 to a drop's $100.

Each leg has a crate on the water about ninety metres before the drop and sixty
metres to the partner's side of the line, so it is not simply on player 1's
nose: both boats race for it, and whoever reaches it first carries it. The
screen gives each boat its distance to the crate ("CARGO  P1 120m  P2 125m"),
so the race is legible from the sofa.
The carrier's boat gets a green radar blip and must stop in the drop ring for
two seconds; the other boat gets the "COVER" distance and keeps the Predators
off. A Predator is sent out from the first leg, and the police switch to
chasing whichever boat has the crate, so the escort has a real job on every leg.
If the carrier's boat is wrecked or left, the crate floats where it was and the
other boat can take it. Player 1 is still the skipper -- the run ends when their
lead boat is lost or they leave it -- but the main job changes hands each leg
depending on who wins the crate. The party's boats can ram or shoot the police
like any other boat. The run ends when the lead boat is lost or wrecked, when
player 1 is out of it for five seconds, or after the third drop, and then the
boats are handed back: partners at their wheels are taken out of the world (as a
change of clothes does), the boats become any other boat on the water, and the
partners come back by the usual rules -- beside player 1 on foot, or after the
boat ride if they are still sailing. A partner who cannot be put anywhere (no
clear water, no room in the pools) waits for a boat ride as they always have.
The party leash stands aside during the run so an escort boat can race for the
crate instead of being towed back to the lead.

The Predators are not passive scenery. Inside 70 m, with the carrier in the arc
ahead of the boat, a Predator fires its twin machine guns at the carrier's hull
in bursts; the escort's job is to take its blind side, ram it, or put the boat
down. Both pursuers get light-blue radar blips, a colour the drop,
the crate and the carrier do not use, and the run's second line names the
nearest live one inside 120 m ("POLICE 48m"), so the warning is on the same line
as the cover distance. A run Predator has a 600-point hull instead of
the stock 1,000, which keeps the one-time SMG kit useful: a concentrated burst
can finish one, and ramming remains the other answer. Each of the two pursuers
is sent once: a destroyed Predator is not replaced, so putting one down buys
the rest of the run. Once a crate is aboard, the carrier is the target; during
the race for it the lead boat is. The escort can work without being shot at,
but is not safe from a crossfire.

Cover is from the boat: the driver has the SMG drive-by and can ram, and getting
out at sea would abandon the cargo and the escort. The run and the cop shift
hand each player a small cover kit once when they start: an Uzi with 60 rounds
if nobody has an SMG (or a partner's SMG kind with at least their count, so the
shared pool is not cut down), or enough to bring an existing SMG back to a
30-round clip or up to a partner's larger count of the same kind. It is not
taken away afterwards. That is how the game treats mission weapons -- a gift
that stays -- and the kit does not hand out a fresh two clips on every repeat
run. The cover SMG is put in hand in a car or boat (or an empty hand), so COVER
works immediately instead of leaving them holding fists. In the cop shift one
player drives and the other shoots from the passenger seat.

The camera holds still for the derby and the toy race (`SteadyView` -- spinning
and twitchy cars would whip the shared view around), and the wall and leash
stand aside for the cop shift, cops & robbers, the smuggling run, the RC race
and King of the Hill (`FreeRoam` -- a pursuit nobody can outrun is not a
pursuit, a run whose escort is towed back cannot race for the crate, a toy race
whose cars are leashed is not a race, and a hill whose ring is smaller than the
tether is not a fight). The cop shift's takedown exemption reaches the crime reporter
through `OnDuty`.

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

- **The minigames' arming gestures are ordinary driving buttons.** The football
  is armed by a one-second hold of the brake with the party parked within
  19.5 m — the ring — and the derby by one press of the horn with the cars
  within 12 m. In traffic that is a ready-up card nobody asked for. It is
  never a mode that starts by itself: the card waits for both players to
  press A, and it goes away again on B or after eight seconds.
- **Nothing clears a mode's arena of traffic or pedestrians**, so a mode
  played in the middle of the city has the city in it. The police no longer
  come for it, but a car can still drive through the ring.
- **No partner is ever a passenger in a boat.** The automatic seat never takes
  one, for the reasons under "Riding along, and driving"; a bike's pillion rides
  without a drive-by, and a boat has no seat to ride in.
- **Boats and trains**: a partner waits out of the world, except on a smuggling
  run, where the run gives them a boat to drive. They can still be a passenger
  in another player's car or pillion on their bike.
- **The wallet and the arsenal are player 1's.** Everyone collects pickups now,
  but weapons and ammo, money, packages and property are credited to player 1
  -- a partner gets the weapon back through the usual mirror -- so a money
  pickup a partner walks over is really player 1's. Health, armour, adrenaline
  and a bribe stay with whoever walked over them. A mode's payout goes there
  too, and the result says so: `P2 WINS   PARTY +$500`. A win that lands in a
  wallet nobody is holding used to read as a win that paid nothing.
- **No remote speaker for the partners.** The speaker code drives one remote,
  and each partner's would need its own stream. Rumble works for everyone
  (gunfire, explosions, the car or bike they are in, thunder).
- **The partners' skins** come from the special-character models, one choice per
  partner since stage 5; the default is player 1's model, which means player 1's
  skin texture too (`RenderPlayerCB` applies slot 0's).
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
- **A partner's ammo readout is under the badge.** The HUD proper shows player 1's
  weapon, which is the shared count; each partner's own gun and its count are
  on their pip, and the engine switching weapon when the pool runs dry is the
  only other notice they get.
- **Dynamic control ownership** — SA let the players hand "control" back and
  forth, with the camera following whoever had it. Here the camera frames the
  party's centroid and player 1 is the anchor. Framing a player's lock target
  is not done either.
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
| run cargo before the drop / pickup radius | `kCargoDistance` / `kCargoRadius`, `CoopModes.cpp` | 90 m / 10 m |
| run drop ring / unload hold | `kDropRadius` / `kUnloadMs`, `CoopModes.cpp` | 35 m / 2 s |
| cover gun: new / refill to | `kCoverGunAmmo` / `kCoverGunFloor`, `CoopModes.cpp` | 60 / 30 rounds |
| player colours | `kCoopColours`, `Hud.cpp` | cyan / pink / green / amber |
