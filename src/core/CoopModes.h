#pragma once

class CEntity;

// Couch co-op's minigames: engine-side modes the party starts in the world,
// as opposed to the game's own scripted missions.  The design is in
// COOP-4-PLAN.md, "Minigames".  Each mode arms from something the players
// deliberately do in the world, and the co-op page's MINIGAMES row
// (CoopMinigames, in Coop.h) switches them off entirely -- it gates arming
// only, so a mode already running is left to finish.
//
// The first one is the cop shift.  A police car or Enforcer with two or more
// players aboard and the siren on puts the party on duty.  Cases spawn a
// suspect car out at arm's length and it drives off; stopping it, wrecking it
// or killing its driver pays, a streak scales the pay, and a suspect that gets
// clear of the leash is a case lost.  While a case is open the takedown is not
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
// The second one is the smuggling run.  A boat with two or more players and
// the horn arms it; three drops are planned along the coast on water path
// nodes, each marked by a radar blip and a corona, and each unloads by holding
// the boat in the ring at rest.  Every drop pays, raises the party's wanted
// level by one, and brings heat: a Predator, the game's own police boat, with
// a cop at the wheel, chasing on the same drive-to-coords machinery.  Deliver
// all three for the cache -- an Uzi into the party's arsenal.  A boat lost,
// wrecked, or left empty for a moment is a run over.
//
// The third is the Bloodring derby.  Two or more players parked in a knot --
// any open ground will do; the ring is drawn around them -- and a honk starts
// a countdown.  Last car running takes the round, first to two rounds takes
// the match.  Weapons are off for the duration, a loaner Bloodring Banger
// appears for anyone who needs one, and the camera holds still: a derby is all
// spinning cars, and a view that turns with one of them turns with all.
//
// The fourth is the toy race.  The party on foot in a knot and somebody jumps:
// RC Bandits are placed in a grid on the spot, a ring of four gates glows
// around them -- each driver's own next gate in their own colour -- and three
// laps take the pot.  A wrecked toy is replaced back at the middle with the
// laps kept, and the driver inside is not drawn, because a normal-sized ped in
// a toy is a normal-sized ped through its roof.
//
// The fifth is the dance-off.  The party on foot in a knot and every one of
// them ducking together: sixty seconds on a spot, a beat that climbs from 700
// milliseconds to 420, and a prompt each -- JUMP, DUCK or the 2 button, drawn
// in the player's own colour at the bottom of the screen.  Hit the prompt
// before the next beat or the combo drops.  Every hit pays and the best score
// takes the pot.  The prompts are real actions, so the dancing is the party's
// own bodies: the game's own dance loops are script data this side of the
// engine cannot name.
//
// The sixth is cops & robbers.  The party on foot in a knot and a press of the
// 2 button: a case appears at their feet and a getaway marker a short run
// away.  Whoever grabs the case is the robber; everyone else is a cop.  Run or
// drive it to the getaway for a point, or pin the carrier for a moment to bust
// them -- the case falls where it fell, and anyone can take it.  First to
// three takes the pot.  The party's wall and leash stand aside for it: a
// pursuit nobody can outrun is not a pursuit.
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

	// Whether a mode is running that wants the camera to hold still rather
	// than turn with a car's nose: the derby and the toy race.
	static bool SteadyView(void);

	// Whether a mode is running that needs the party able to separate: cops &
	// robbers, which the session's wall and leash would otherwise strangle.
	static bool FreeRoam(void);

	// The minigames' screen marks, called by the HUD with the other co-op
	// ones: the dance prompts and the cops & robbers scores, when a round is
	// on.
	static void DrawHud(void);
};
