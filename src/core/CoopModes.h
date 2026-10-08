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
};
