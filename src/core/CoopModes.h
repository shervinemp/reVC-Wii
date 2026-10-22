#pragma once

class CEntity;
class CPlayerPed;
class CVehicle;
class CVector;

// Couch co-op's minigames: engine-side modes the party starts in the world,
// as opposed to the game's own scripted missions.  The design is in
// COUCH-COOP-DESIGN.md, "Minigames".  Each mode arms from something the players
// deliberately do in the world, and the co-op page's MINIGAMES row
// (CoopMinigames, in Coop.h) switches them off entirely -- it gates arming
// only, so a mode already running is left to finish.  The cop shift has no
// end of its own, so with the switch off it plays out the case it has open
// and then stands down.
//
// The first one is the cop shift.  A police car or Enforcer with two or more
// players aboard and the siren on puts the party on duty.  Cases spawn a
// suspect car out at arm's length and it drives off; stopping it, wrecking it
// or killing its driver pays, a streak scales the pay, and a suspect that gets
// clear of the leash is a case lost.  A case runs for forty-five seconds and
// the next one comes straight after, because a shift that is eight seconds of
// nothing between every case is a shift nobody is waiting for.  While a case is open the takedown is not
// a crime -- see CEventList::RegisterEvent -- so the shift does not write its
// own wanted level.
//
// Everything here is the script's own machinery.  The suspect is
// COMMAND_CREATE_CAR's recipe: CAutomobile(MISSION_VEHICLE), locked, joined to
// the road system, with a MISSION_CHAR driver warped in by
// CPed::WarpPedIntoCar and sent off by JoinCarWithRoadSystemGotoCoors, exactly
// as COMMAND_CAR_GOTO_COORDINATES does it.  A resolved case takes the same
// doors the scripts use: CTheScripts::RemoveThisPed and the DELETE_CAR recipe
// after a takedown, and CleanUpThisPed / CleanUpThisVehicle to let an escaped
// suspect melt back into the traffic.  Nothing is spawned that the engine does
// not already know how to own.
//
// The second one is the smuggling run.  Player 1 at the wheel of a boat and the
// horn arms it.  A boat cannot carry the party -- the game gives every boat one
// passenger seat at most, on top of the driver's -- so the rest of the party
// are each given a boat of their own: as a partner comes into the world with
// player 1 at sea, CCoop asks EscortBoat, which makes a boat of the lead's own
// model on the water beside it, and the partner is put at its wheel.  They
// are the crew while they keep within range of the lead boat; the party leash
// stands aside so an escort can race the lead to a crate.  Three drops are
// planned along the coast on water path nodes, each marked by a radar blip and
// a corona, and each unloads by holding the lead boat in the ring at rest.
// Every drop pays (more for each boat of the crew -- measured against the boat
// carrying the crate, which is the boat the escort is told to stay near),
// raises the party's wanted level by one, and brings heat: a Predator, the
// game's own police boat, with a cop at the wheel, chasing the lead boat on
// the same drive-to-coords machinery.  The Predators only ever shoot the
// carrier, so the escort's job is the police: a police boat that goes down
// pays, and the screen says so.  Deliver all three for the cache -- an Uzi into the party's
// arsenal.  The lead boat lost, wrecked, or left by player 1 for a moment is a
// run over, and the party's boats are handed back: partners at their wheels
// are taken out of the world, and come back by the usual rules.
//
// The third is the Bloodring derby.  Two or more players parked in a knot --
// any open ground will do; the ring is drawn around them -- and a honk held
// for 0.6 s starts a countdown.  The horn is held rather than tapped because
// it is an ordinary driving button, and a tap on it at a traffic light used
// to stop the world to ask two people whether they wanted to play.  Last car
// running takes the round, first to two rounds takes
// the match.  Weapons are off for the duration, a loaner Bloodring Banger
// appears for anyone who needs one, and the camera holds still: a derby is all
// spinning cars, and a view that turns with one of them turns with all.
// Between rounds the party has ten seconds to get back into a car and drive
// to the ring, and the round waits for them rather than ending the match: a
// first-to-two match is not lost because somebody could not find their door.
//
// The modes are heard as well as seen.  A mode that counts down counts down
// out loud, with the game's own three-two-one-go, and the moments that matter
// have their own sounds: the money for a case caught, the chime for a goal,
// the arrest for a bust, the pay for a drop.  They are played through the
// front end, so they come from the screen and not from anybody's feet: one
// screen, one room, one sound.
//
// The fourth is the toy race.  The party on foot in a knot and somebody jumps:
// RC Bandits are placed in a grid on the spot, a ring of four gates glows
// around them from the moment the grid opens -- each driver's own next gate
// in their own colour -- and three laps take the pot.  A wrecked toy is replaced beside its racer with the laps
// kept, and the driver inside is not drawn, because a normal-sized ped in a
// toy is a normal-sized ped through its roof.
//
// The fifth is the dance-off.  The party on foot in a knot and every one of
// them ducking together: sixty seconds on a spot, a beat that climbs from a
// second to 650 milliseconds, and a prompt each -- jump, duck or the nod (not
// the nod for a dancer with a car in reach: it would get them in), each
// printed as the player's own controller's button, in their colour at the
// bottom of the screen, with the action under it.  The chip comes up a beat
// before the beat it is for, a bar inside it drains towards that beat, and
// the beat itself is heard: the game's own clock tick, once a beat, through
// the front end, so it comes from the screen and both players hear it at
// once.  There is no music this side of the engine that a minigame can play
// on cue, and a rhythm game that cannot show or sound the beat is a guessing
// game.  A press has to land within a window of the beat -- a fifth of the
// beat, which closes as the tempo climbs -- and the press is graded perfect,
// good, early or late, with the grade on screen for a moment.  The game never
// asks for a button the player is already holding, so holding the duck down
// does not cost them a third of the prompts and a mash scores nothing.  Every
// hit pays a little and the best score takes the pot.  The prompts are real actions, so the dancing is the party's
// own bodies: the game's own dance loops are script data this side of the
// engine cannot name.
//
// The sixth is cops & robbers.  The party on foot in a knot and the nod held
// for 0.6 s -- not a tap, which is as likely to be meant for something else,
// and not a press from a player with a car in reach, because the nod is also
// the enter-vehicle button and that press is somebody getting in: a case
// appears at their feet and a getaway marker a short run away.  Whoever grabs the case is the robber; everyone else is a cop.  Run or
// drive it to the getaway for a point, or pin the carrier for a moment to bust
// them -- the case falls where it fell, and after a bust it lies dead for
// three seconds: nobody can pick it up and nobody can be pinned.  The busted
// player is then stood off the case for six seconds, so the cop who busted
// them takes it and the two of them have swapped sides.  The sides changing
// with every bust is the whole point of a mode where the same two people
// keep chasing each other.  The
// screen says who busted whom.  First to three takes the pot.  The
// party's wall and leash stand aside for it: a pursuit nobody can outrun is
// not a pursuit.
//
// The seventh is Caddy football.  A Caddy with nobody in it is the ball; the
// party's own cars push it.  Two players get two goals and a 1v1, four get
// 2v2, and three get three goals in a triangle, each player defending the
// nearest one.  One player holds the brake in a car to start; ramming only;
// first to three.  A ball that has not moved for six seconds is wedged --
// under a car, pinned in a corner -- and goes back to the middle.  The brake,
// not the nod or the horn: the nod is the
// exit-vehicle button in a car, and the horn already starts the derby.  The
// camera holds still and the pitch is built wherever the party stopped.
//
// The eighth is King of the Hill.  Every present player holds aim on foot,
// and a ring is drawn on open ground near them: a street is not an arena, so
// the ring moves to the nearest clear patch that is not a roadway.  The hill
// is a point at the centre of that ring, and the player nearest it holds it.
// The hold belongs to the hill: it is lost when the hill changes hands or is
// left empty, so the round is decided by who is holding the point when the
// bell goes.  A player in a car is a spectator, not a second holder.  Player-
// on-player damage is forced on for the round whatever the friendly-fire
// toggle says, because a hill nobody can take from anybody is not a hill.  A
// downed or arrested partner is brought back at the body after four seconds,
// so a fight does not thin the party out; the ordinary revive is still there
// for anyone who reaches them sooner.  Player 1 is the mode's too: the
// wasted/hospital sequence is held off and he comes back at the body on the
// same clock, with the party still in the session.  Ninety seconds; the
// holder at the bell wins.
//
// The toy race, the dance-off, cops & robbers, Caddy football, Bloodring and
// King of the Hill do not arm while anyone in the party is wanted: they start
// from a jump, a duck, a press of the nod, a horn or a held brake, the things
// a party being chased does anyway.  The cop shift is held back too -- the
// law's view of the party is that nobody starts a shift with a record -- but
// the smuggling run is not: it is about the police, and it brings its own
// heat.
//
// And while a mode is running, the party's crimes are not reported to the
// police at all: the event system is what the games themselves generate --
// a car through a pedestrian in the middle of a derby -- and it is what
// brought the police to a round that was meant to be a race.  A script's own
// crimes, an alarm or a bank robbery, are not the event system's and still
// count.  The cop shift is the exception, because there the police are the
// point: the takedown is the job, and anything else the party does there is a
// crime like anywhere else.
class CCoopModes
{
public:
	// Forgets everything.  For a new game, a loaded game and shutdown.
	static void Init(void);
	// Once a frame, after CCoop::Update, so the session state is this
	// frame's.
	static void Update(void);

	// Whether a crime report is part of an open case and so not a crime: the
	// victim is the suspect, or it is gunfire from a player close to the
	// case.  Called from CEventList::RegisterEvent for events a player
	// caused.
	static bool OnDuty(CEntity *criminal, CEntity *victim, bool gunfire);

	// A boat for a partner to take the wheel of, while the smuggling run is on
	// and player 1 is sailing the run's boat.  Asked by CCoop when it is about to
	// bring a partner into the world and finds player 1 in a boat, where there
	// would otherwise be no seat for them: a mission boat of the lead's own
	// model, made in the water beside it, empty, and the caller seats the
	// partner at its wheel.  The run keeps hold of it (it is a mode vehicle,
	// so the save leaves it out) and hands it back when the run is over, with
	// any partner still in it taken out of the world first.  Nil when the run
	// is not on, the boat is not the run's, or there is no clear water for it.
	// The same boat comes back until it is wrecked, taken, or left behind.
	static CVehicle *EscortBoat(int player, CVehicle *lead);

	// Get a player out of an RC toy without the engine's car-door exit path.
	// The RC Bandit has no door dummies or door frames, so SetExitCar's normal
	// animation/collision route is not safe in it.  This detaches the ped where
	// it stands, puts them on clear ground beside the toy and leaves the race
	// to respawn the toy on its own clock.  Called by the player-1 and partner
	// input paths before they hand the exit button to SetExitCar, and by
	// SetExitCar itself as the last line of defence.  False when the ped is not
	// in one of the race's toys.
	static bool LeaveRcCar(CPlayerPed *ped);

	// Whether this is one of the remote-control models.  They have no normal
	// door frames and are not enterable by the ordinary car search, even after
	// the race has handed one back to the world.
	static bool IsRcVehicle(const CVehicle *vehicle);

	// The driverless Caddy that is the football.  It is locked and is not a car
	// anybody may enter or steal.
	static bool IsPitchBall(const CVehicle *vehicle);

	// The menu's shortcuts into the modes.  Each one teleports the party to a
	// clear piece of the map and puts them in whatever the mode needs -- cars
	// for the derby and Caddy football, a police car for the shift, boats for
	// the run -- so the gesture is only the way to start it in the world.
	enum eMenuMode {
		MENU_SHIFT,
		MENU_RUN,
		MENU_DERBY,
		MENU_RC,
		MENU_DANCE,
		MENU_CNR,
		MENU_PITCH,
		MENU_KOTH
	};
	static bool MenuStart(int mode);

	// Whether a mode is running that wants the camera to hold still rather
	// than turn with a car's nose: the derby, the toy race, Caddy football and
	// King of the Hill.
	static bool SteadyView(void);

	// Whether a mode is running that needs the party able to separate: the cop
	// shift and cops & robbers, whose pursuits the leash would otherwise
	// catch; the smuggling run, whose escort boats have to race the lead to a
	// crate; the RC race, whose cars have to get away from player 1 to race;
	// and King of the Hill, whose 12 m ring leaves two players at opposite
	// edges past the walking tether.  The wall would hold a partner on foot and
	// the leash would pull a partner's vehicle back mid-run.
	static bool FreeRoam(void);

	// Where the action is in Cops & Robbers: the case carrier while it is
	// carried, otherwise the loose case.  Cam.cpp leans the shared target this
	// way once the party is split, so a pursuit is not framed as a picnic.
	// False in every other mode.
	static bool CameraHint(CVector &target);

	// The fixed arena a steady-view mode is played in: the centre the shared
	// camera should frame, and the radius that has to stay on screen.  Used by
	// Cam.cpp for the derby, RC race, Caddy football and King of the Hill.
	// False in every other mode.
	static bool ArenaFocus(CVector &centre, float &radius);

	// True while King of the Hill is running and player 1 is dead but the
	// mode's four-second revive is still owed.  CGameLogic and the co-op
	// session read this so the ordinary wasted/hospital flow does not take the
	// party down with him.  False for the arrest path and after the bell.
	static bool KothLeadDown(void);

	// Whether a mode is running that forces player-on-player damage on,
	// whatever the friendly-fire toggle says: King of the Hill.  Asked by
	// CCoop::FriendlyFireBlocked.
	static bool ForceFriendlyFire(void);
	// How long a downed partner's bar should count, for the HUD.  King of the
	// Hill brings them back sooner than the ordinary bleed-out.
	static int DownedBleedMs(int normal);

	// Whether a vehicle is one a mode made for itself and means to hand back: a
	// toy, a loaner, the suspect's car, a smuggler's pursuer.  They are all
	// mission vehicles, which is what the game saves when they stand empty, so
	// the save leaves them out; loaded back they would be orphans nothing in the
	// world would ever clear away.
	static bool IsModeVehicle(const CVehicle *vehicle);
	// Couch co-op: the session's scoreboard.  How many modes each player has
	// won -- theirs alone, where the money is everybody's.  Counted from the
	// moment a mode is decided, and it lasts the session.
	static int TrophyCount(int player);
	static void AwardTrophy(int player);

	// The minigames' screen marks, called by the HUD with the other co-op
	// ones: the headline and countdown at the top (the cop shift's streak and
	// what its last case came to among them), the bars along the bottom, a
	// cell to a round, a point or a lap, each with its player's name over it, the
	// dance prompts, and the first time a mode arms in a session a line
	// saying how it is played -- while a mode is on and for the moment after.
	static void DrawHud(void);
};
