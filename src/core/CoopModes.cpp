#include "common.h"

#include "CoopModes.h"

#include "Automobile.h"
#include "AutoPilot.h"
#include "Boat.h"
#include "CarCtrl.h"
#include "CivilianPed.h"
#include "Coop.h"
#include "CopPed.h"
#include "Coronas.h"
#include "General.h"
#include "Hud.h"
#include "maths.h"
#include "ModelIndices.h"
#include "Pad.h"
#include "PathFind.h"
#include "Ped.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Pools.h"
#include "Population.h"
#include "Radar.h"
#include "Script.h"
#include "Streaming.h"
#include "Text.h"
#include "Timer.h"
#include "Vehicle.h"
#include "Wanted.h"
#include "WeaponInfo.h"
#include "World.h"
#include "Zones.h"
#include "config.h"
#ifdef NINTENDO_WII
#include "WiiPad.h"
#include "WiiTrace.h"
#endif

// Transitions only, the same budget Coop.cpp holds to: a session that is
// working writes a handful of these lines and no more.
#ifdef NINTENDO_WII
#define COOP_LOG(...) WiiTraceReport(__VA_ARGS__)
#else
#define COOP_LOG(...) debug(__VA_ARGS__)
#endif

namespace
{

// --- the cop shift's numbers -------------------------------------------------

const uint32 kShiftStartDelayMs = 2000;	// beat after the siren goes on
const uint32 kCaseCooldownMs = 4000;	// between cases
const uint32 kCaseTimeoutMs = 90000;	// a case older than this got away
const float kCaseLeash = 250.0f;	// ...as did one further than this
const float kSpawnDistance = 90.0f;	// how far out a case starts
const uint32 kRetargetMs = 5000;	// routine re-flee interval
const float kCloseRange = 70.0f;	// crew this close re-routes the suspect
const uint32 kCloseRetargetMs = 2500;
const float kStopRange = 12.0f;		// "cornered" needs a player this close
// The speeds in this file are game speeds, not metres per second:
// GetMoveSpeed() is in world units per game step and 1.0 is 50 m/s (see
// CarCtrl.h), so they are written as a number of metres per second times
// METERS_PER_SECOND_TO_GAME_SPEED.  A suspect at cruise is about 0.42; "held
// still" is a few metres per second at the most.
const float kStopSpeed = 2.0f * METERS_PER_SECOND_TO_GAME_SPEED;
const uint32 kStopHoldMs = 2000;
const uint32 kCleanupDelayMs = 4000;	// how long a resolved case lingers
const float kGunfireRange = 60.0f;	// on duty: gunfire this near the case
const uint32 kShiftResultMs = 2000;	// how long what a case came to stays on screen
const int kCaseCars[] = { MI_SENTINEL, MI_STALLION, MI_VOODOO, MI_SABRE };

// What the screen says about the shift's last turn, for kShiftResultMs.
enum eShiftResult
{
	SHIFT_RESULT_NONE,
	SHIFT_RESULT_CAUGHT,	// a case closed, with the pay
	SHIFT_RESULT_AWAY,	// a case lost
	SHIFT_RESULT_SIREN,	// the siren went off
	SHIFT_RESULT_STOOD_DOWN	// the crew left the cruiser, or the switch went off
};

// --- the smuggling run's numbers ---------------------------------------------

const uint32 kRearmMs = 8000;		// after a run, before the horn arms again
const int kDrops = 3;
const float kLegDistance = 320.0f;	// water between drops
const float kDropSearch = 300.0f;	// how far a leg may look for water
const float kDropRadius = 35.0f;	// in the ring...
const float kDropSpeed = 3.0f * METERS_PER_SECOND_TO_GAME_SPEED;	// ...and this slow (3 m/s) to unload
// Three drops that planned out within this much of the boat, or of each other,
// would pay out where the boat already is: a drop that near is not a drop.
const float kMinDropSeparation = 150.0f;
const uint32 kUnloadMs = 2000;
const float kPursuerDistance = 130.0f;	// heat spawns this far out
const uint32 kPursuerRetargetMs = 3000;
const uint32 kCrewGraceMs = 5000;

// The shift, and the case it may have open.  The suspect is held as
// registered references, so the engine nils them if anything ever deletes
// either half; everything below treats nil as "gone".
struct ShiftState
{
	bool active;
	uint32 streak;
	uint32 nextCaseAt;
	uint32 crewLostAt;
	// an open case
	bool caseActive;
	bool spawnPending;
	uint32 spawnRequestAt;
	int spawnCarModel;
	int spawnPedModel;
	uint32 caseStartAt;
	uint32 stoppedSince;
	uint32 nextRetargetAt;
	uint32 closeRetargetAt;
	// a resolved case, still on the stage for a moment
	bool cleanupPending;
	bool escaped;
	uint32 cleanupAt;
	// what the last case or the end of the shift came to, for the screen
	uint32 resultAt;
	int resultKind;
	uint32 resultPay;
	CVehicle *suspectCar;
	CPed *suspectPed;
};
ShiftState s_shift;

// The run: the party's boat, the three drops planned from where it armed, and
// the two police boats the heat brings.  Same rule as the shift: every entity
// is a registered reference, and nil means gone.
struct RunState
{
	bool active;
	uint32 rearmAt;
	int leg;
	CVector dropPos[kDrops];
	CVector drop;
	int32 blip;
	uint32 unloadSince;
	uint32 crewLostAt;
	uint32 nextRetargetAt;
	CVehicle *boat;
	CVehicle *pursuer[2];
	CPed *pursuerDriver[2];
};
RunState s_run;

// --- the Bloodring derby's numbers -------------------------------------------

const float kDerbyGatherRange = 12.0f;	// parked this close to arm it
const float kDerbyParkedSpeed = 1.5f * METERS_PER_SECOND_TO_GAME_SPEED;	// "parked": 1.5 m/s or less
const float kDerbyRadius = 14.0f;	// the ring itself
const int kDerbyRingMarks = 10;		// coronas round it
const float kDerbyJoinRange = 24.0f;	// cars this close to the middle at the bell are in
const uint32 kDerbyCountdownMs = 3000;
const uint32 kDerbyOutGraceMs = 3000;	// outside the ring this long = out
const uint32 kDerbyFootGraceMs = 5000;	// out of the car this long = out
const uint32 kDerbyIntermissionMs = 6000;
const uint32 kDerbyRoundCapMs = 180000;	// a round that will not end
const uint32 kDerbyRearmMs = 10000;
const int kDerbyWinsNeeded = 2;

// The derby: a ring drawn wherever the party parked, the running order, and
// the cars the mod provided for anyone who needed one.
struct DerbyState
{
	bool active;
	bool counting;
	bool finished;
	uint32 countdownAt;
	uint32 finishAt;
	uint32 rearmAt;
	CVector centre;
	CVector ring[kDerbyRingMarks];
	int round;
	uint32 roundStartAt;
	int roundWins[NUMPLAYERS];
	bool running[NUMPLAYERS];
	uint32 outSince[NUMPLAYERS];
	uint32 footSince[NUMPLAYERS];
	int matchWinner;
	CVehicle *loaner[NUMPLAYERS];
};
DerbyState s_derby;

// --- the RC race's numbers ---------------------------------------------------

const int kRcGates = 4;
const int kRcLaps = 3;
const float kRcRadius = 16.0f;		// the gate ring
const float kRcGateRange = 7.0f;
const float kRcGatherRange = 12.0f;	// the knot that arms it
const float kRcSeatedSpeed = 1.5f * METERS_PER_SECOND_TO_GAME_SPEED;	// "still": 1.5 m/s or less
const uint32 kRcFootGraceMs = 5000;	// a racer on foot waits this long for a toy before it is brought
const uint32 kRcGridHoldMs = 2000;	// seated and still this long...
const uint32 kRcCountdownMs = 3000;	// ...then the lights
const uint32 kRcJumpWindowMs = 1500;	// two or more jump together to arm it
const uint32 kRcGridTimeoutMs = 30000;
const uint32 kRcLingerMs = 4000;
const uint32 kRcRearmMs = 10000;
const int kRcPot = 300;

// The toy race: RC Bandits placed in a grid wherever the party jumped, a ring
// of four gates, three laps.
struct RcState
{
	bool active;
	bool grid;
	bool counting;
	bool finished;
	uint32 gridAt;
	uint32 gridReadyAt;
	uint32 countdownAt;
	uint32 finishAt;
	uint32 rearmAt;
	CVector centre;
	CVector forward;
	CVector gate[kRcGates];
	int nextGate[NUMPLAYERS];
	int lap[NUMPLAYERS];
	uint32 lastJump[NUMPLAYERS];
	uint32 footSince[NUMPLAYERS];
	int winner;
	CVehicle *car[NUMPLAYERS];
};
RcState s_rc;

// --- the dance-off's numbers -------------------------------------------------

const float kDanceGatherRange = 12.0f;	// the knot that arms it
const float kDanceFloor = 6.0f;		// the stage ring
const float kDanceLeaveRange = 8.0f;	// further out than this and you are out of it
const uint32 kDanceRoundMs = 60000;
const uint32 kDanceGestureWindowMs = 1500;	// everyone ducks together
const uint32 kDanceRearmMs = 10000;
const uint32 kDanceLingerMs = 4000;
const uint32 kDanceBeatSlowMs = 1000;	// the tempo climbs...
const uint32 kDanceBeatFastMs = 650;	// ...to here by the time the round ends
const int kDanceHitPay = 10;
const int kDanceWinPot = 200;

enum eDancePrompt
{
	DANCE_JUMP,
	DANCE_DUCK,
	DANCE_TWO,
	DANCE_PROMPTS
};

// The dance-off: the party bobbing on a spot to a beat that keeps climbing.
// The prompts are real actions -- jump, duck, the nod -- so the dancing
// is the party's own bodies; the game's own dance loops are script data this
// side of the engine cannot name.
struct DanceState
{
	bool active;
	bool finished;
	uint32 startAt;
	uint32 beatAt;
	uint32 finishAt;
	uint32 rearmAt;
	uint32 lastDuck[NUMPLAYERS];
	CVector centre;
	int prompt[NUMPLAYERS];
	bool promptHit[NUMPLAYERS];
	uint32 hitFlash[NUMPLAYERS];
	uint32 missFlash[NUMPLAYERS];
	int score[NUMPLAYERS];
	int combo[NUMPLAYERS];
	int winner;
	bool triangleHeld[NUMPLAYERS];
};
DanceState s_dance;

// --- cops & robbers' numbers -------------------------------------------------

const float kCnRGatherRange = 12.0f;	// the knot that arms it
const float kCnRGetawayDist = 24.0f;	// the getaway marker from the middle
const float kCnRGrabRange = 2.5f;	// on foot...
const float kCnRGrabRangeCar = 4.5f;	// ...and from a car
const float kCnRDeliverRange = 4.5f;
const float kCnRBustRange = 5.0f;
const uint32 kCnRBustHoldMs = 1500;
const uint32 kCnRBustLockMs = 2000;	// a busted carrier may not take the case back for this long
const uint32 kCnRNoteMs = 2000;		// "P3 BUSTED P1" stays on screen this long
const float kCnRArena = 34.0f;		// beyond this the case comes back
const uint32 kCnROutGraceMs = 3000;
const int kCnRWinScore = 3;
const uint32 kCnRCapMs = 300000;
const uint32 kCnRLingerMs = 4000;
const uint32 kCnRRearmMs = 12000;
const int kCnRPot = 400;

// Cops & robbers: the case is a position, not an object.  The briefcase model
// would need creating, carrying and attaching, and the corona-and-blip the
// other modes already speak in says where the score is just as well.
struct CnRState
{
	bool active;
	bool finished;
	uint32 startAt;
	uint32 finishAt;
	uint32 rearmAt;
	CVector centre;
	CVector getaway;
	CVector casePos;
	bool carried;
	int carrier;
	int score[NUMPLAYERS];
	uint32 bustSince;
	int bustBy;
	// A bust drops the case at the carrier's feet, where they would be the
	// nearest player and take it straight back: the busted player is kept off
	// it for kCnRBustLockMs, so it is up for the others -- and the screen
	// says who busted whom.
	int lockPlayer;
	uint32 lockUntil;
	int noteBuster;
	int noteBusted;
	uint32 noteAt;
	uint32 outSince;
	int winner;
	int32 caseBlip;
	int32 getawayBlip;
	bool triangleHeld[NUMPLAYERS];
};
CnRState s_cnr;

// --- what a mode tells the party when it arms --------------------------------

// Every mode starts from something the party does in passing -- a honk, a pair
// of jumps, a tap of the nod -- and the help box's mode name is all the game
// says about it, for a second and a half.  So the first time a mode arms in a
// session it also puts one line on the screen, a few seconds long, saying how
// it is played and how it ends.  It is drawn, not told through the help box:
// that is the one-and-a-half-second "quick" box, and it waits behind whatever
// else is in it.  A rematch is not interrupted by it again; the flags go with
// the session (see CCoopModes::Update).
enum eExplainer
{
	EXPLAIN_SHIFT,
	EXPLAIN_RUN,
	EXPLAIN_DERBY,
	EXPLAIN_RC,
	EXPLAIN_DANCE,
	EXPLAIN_CNR,
	EXPLAIN_COUNT
};

const uint32 kExplainMs = 4500;

struct ExplainState
{
	bool seen[EXPLAIN_COUNT];
	int showing;
	uint32 until;
};
ExplainState s_explain;

void
ShowExplainer(int mode, uint32 now)
{
	if(s_explain.seen[mode])
		return;
	s_explain.seen[mode] = true;
	s_explain.showing = mode;
	s_explain.until = now + kExplainMs;
}

// One world mode at a time: every one of these arms from something the party
// does in the world, and two at once would be two modes fighting over the same
// players.
bool
ModeActive(void)
{
	return s_shift.active || s_run.active || s_derby.active || s_rc.active || s_dance.active || s_cnr.active;
}

// Where a player is for these purposes: their car while they are in one, so a
// crew chasing in cars is measured car to car.
CVector
PlayerCoors(int player)
{
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	if(ped == nil)
		return CVector(0.0f, 0.0f, 0.0f);
	if(ped->bInVehicle && ped->m_pMyVehicle != nil)
		return ped->m_pMyVehicle->GetPosition();
	return ped->GetPosition();
}

int
CountPlayersIn(CVehicle *vehicle)
{
	int count = 0;
	for(int player = 0; player < NUMPLAYERS; player++){
		CPlayerPed *ped = CCoop::GetPlayerPed(player);
		if(ped != nil && ped->bInVehicle && ped->m_pMyVehicle == vehicle)
			count++;
	}
	return count;
}

// Whether a player is in the vehicle or on the way into it.  Deleting a
// vehicle takes its occupants with it -- ~CVehicle flags them to be destroyed --
// so a mode that means to delete one asks this first.
bool
PlayerUsingVehicle(CVehicle *vehicle)
{
	if(CountPlayersIn(vehicle) > 0)
		return true;
	for(int player = 0; player < NUMPLAYERS; player++){
		CPlayerPed *ped = CCoop::GetPlayerPed(player);
		if(ped != nil && ped->m_carInObjective == vehicle &&
		   (ped->m_nPedState == PED_SEEK_CAR || ped->m_nPedState == PED_ENTER_CAR || ped->m_nPedState == PED_CARJACK))
			return true;
	}
	return false;
}

// Whether the nod would put this player into a vehicle.  On foot the nod is
// the enter-vehicle button: CPlayerInfo::Process (and CCoop for a partner)
// takes the nearest car within ten metres on the flat that is neither wrecked
// nor on its roof.  This is that reach, a metre generous.  A press with a car
// in reach is somebody getting in, not somebody starting a game.
bool
VehicleInReach(CPlayerPed *ped)
{
	const CVector pos = ped->GetPosition();
	for(int i = CPools::GetVehiclePool()->GetSize() - 1; i >= 0; i--){
		CVehicle *vehicle = CPools::GetVehiclePool()->GetSlot(i);
		if(vehicle == nil || !vehicle->bUsesCollision)
			continue;
		if(vehicle->GetStatus() == STATUS_WRECKED || vehicle->GetStatus() == STATUS_TRAIN_MOVING)
			continue;
		if(vehicle->GetUp().z <= 0.3f && !vehicle->IsBike())
			continue;
		const CVector centre = vehicle->GetBoundCentre();
		if(Abs(centre.z - pos.z) > 4.0f)
			continue;
		if((centre - pos).Magnitude2D() <= 11.0f)
			return true;
	}
	return false;
}

// A law car the party is riding in -- two or more players, a player driving.
// The siren is asked separately: it is both how a shift arms and how it is
// stood down.
CVehicle *
FindCrewedLawCar(bool requireSiren)
{
	CVehicle *best = nil;
	int bestCount = 0;
	for(int player = 0; player < NUMPLAYERS; player++){
		CPlayerPed *ped = CCoop::GetPlayerPed(player);
		if(ped == nil || !ped->bInVehicle || ped->m_pMyVehicle == nil)
			continue;
		CVehicle *veh = ped->m_pMyVehicle;
		if(!veh->bIsLawEnforcer || !veh->IsCar())
			continue;
		if(veh->pDriver == nil || !veh->pDriver->IsPlayer())
			continue;
		if(requireSiren && !veh->m_bSirenOrAlarm)
			continue;
		const int count = CountPlayersIn(veh);
		if(count >= 2 && count > bestCount){
			bestCount = count;
			best = veh;
		}
	}
	return best;
}

float
NearestPlayerDist(const CVector &pos)
{
	float best = 1000000.0f;
	for(int player = 0; player < NUMPLAYERS; player++){
		CPlayerPed *ped = CCoop::GetPlayerPed(player);
		if(ped == nil)
			continue;
		const float dist = (PlayerCoors(player) - pos).Magnitude();
		if(dist < best)
			best = dist;
	}
	return best;
}

CEntity *
NearestPlayerPed(const CVector &pos)
{
	CPlayerPed *best = nil;
	float bestDist = 1000000.0f;
	for(int player = 0; player < NUMPLAYERS; player++){
		CPlayerPed *ped = CCoop::GetPlayerPed(player);
		if(ped == nil)
			continue;
		const float dist = (ped->GetPosition() - pos).Magnitude();
		if(dist < bestDist){
			bestDist = dist;
			best = ped;
		}
	}
	return best;
}

CVector
CrewCentre(void)
{
	CVector sum(0.0f, 0.0f, 0.0f);
	int count = 0;
	for(int player = 0; player < NUMPLAYERS; player++){
		if(CCoop::GetPlayerPed(player) == nil)
			continue;
		sum += PlayerCoors(player);
		count++;
	}
	if(count == 0)
		return CVector(0.0f, 0.0f, 0.0f);
	return sum / (float)count;
}

bool
FindCarNodeNear(const CVector &point, float radius, CVector &out)
{
	const int32 node = ThePaths.FindNodeClosestToCoors(point, PATH_CAR, radius);
	if(node < 0)
		return false;
	out = ThePaths.m_pathNodes[node].GetPosition();
	return true;
}

// Sends the suspect away from the crew: a destination down the road in the
// direction it is already leaving in, re-pathed on a clock.  This is
// COMMAND_CAR_GOTO_COORDINATES' recipe, nothing more -- the suspect has no
// idea where the players are, it simply keeps going.
void
DriveSuspectAway(uint32 now)
{
	CVehicle *car = s_shift.suspectCar;
	if(car == nil)
		return;
	const CVector carPos = car->GetPosition();
	CVector away = carPos - CrewCentre();
	away.z = 0.0f;
	if(away.Magnitude() < 1.0f)
		away = car->GetForward();
	away.Normalise();

	const CVector destPoint = carPos + away * 800.0f;
	CVector dest;
	if(!FindCarNodeNear(destPoint, 400.0f, dest))
		dest = destPoint;

	if(CCarCtrl::JoinCarWithRoadSystemGotoCoors(car, dest, false))
		car->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS_STRAIGHT;
	else
		car->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS;
	car->SetStatus(STATUS_PHYSICS);
	car->bEngineOn = true;
	car->AutoPilot.m_nCruiseSpeed = car->AutoPilot.m_fMaxTrafficSpeed = 25;
	car->AutoPilot.m_nAntiReverseTimer = now;
	s_shift.nextRetargetAt = now + kRetargetMs;
}

// COMMAND_CREATE_CAR and COMMAND_CREATE_CHAR, then
// COMMAND_WARP_CHAR_INTO_CAR: the suspect is a mission car and a mission ped
// by the book, so every system that protects or cleans up script creations
// protects and cleans up this one.
bool
SpawnSuspect(uint32 now)
{
	const CVector crew = CrewCentre();

	CVector spawnPos;
	bool found = false;
	for(int attempt = 0; attempt < 4 && !found; attempt++){
		const float angle = DEGTORAD((float)CGeneral::GetRandomNumberInRange(0, 360));
		const CVector dir(Cos(angle), Sin(angle), 0.0f);
		found = FindCarNodeNear(crew + dir * kSpawnDistance, 45.0f, spawnPos);
	}
	if(!found)
		return false;

	CAutomobile *car = new CAutomobile(s_shift.spawnCarModel, MISSION_VEHICLE);
	if(car == nil)
		return false;
	spawnPos.z += 4.0f;
	car->SetPosition(spawnPos);
	CTheScripts::ClearSpaceForMissionEntity(spawnPos, car);
	car->SetStatus(STATUS_ABANDONED);
	car->bIsLocked = true;
	CCarCtrl::JoinCarWithRoadSystem(car);
	car->AutoPilot.m_nCarMission = MISSION_NONE;
	car->AutoPilot.m_nTempAction = TEMPACT_NONE;
	car->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
	car->AutoPilot.m_nCruiseSpeed = car->AutoPilot.m_fMaxTrafficSpeed = 25;
	car->AutoPilot.m_nCurrentLane = car->AutoPilot.m_nNextLane = 0;
	car->bEngineOn = true;
	car->m_nZoneLevel = CTheZones::GetLevelFromPosition(&spawnPos);
	car->bHasBeenOwnedByPlayer = true;

	CVector away = spawnPos - crew;
	away.z = 0.0f;
	away.Normalise();
	car->GetForward() = CVector(away.x, away.y, 0.0f);
	car->GetRight() = CVector(away.y, -away.x, 0.0f);
	car->GetUp() = CVector(0.0f, 0.0f, 1.0f);
	CWorld::Add(car);

	CCivilianPed *ped = new CCivilianPed(PEDTYPE_CIVMALE, s_shift.spawnPedModel);
	if(ped == nil){
		CWorld::Remove(car);
		CWorld::RemoveReferencesToDeletedObject(car);
		delete car;
		return false;
	}
	ped->CharCreatedBy = MISSION_CHAR;
	ped->bRespondsToThreats = false;
	ped->bAllowMedicsToReviveMe = false;
	ped->bIsPlayerFriend = false;
	CVector pedPos = car->GetPosition();
	pedPos.z += 1.0f;
	ped->SetPosition(pedPos);
	ped->SetOrientation(0.0f, 0.0f, 0.0f);
	CWorld::Add(ped);
	ped->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pedPos);
	++CPopulation::ms_nTotalMissionPeds;
	ped->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, car);
	ped->WarpPedIntoCar(car);

	car->RegisterReference((CEntity**)&s_shift.suspectCar);
	ped->RegisterReference((CEntity**)&s_shift.suspectPed);
	s_shift.suspectCar = car;
	s_shift.suspectPed = ped;

	DriveSuspectAway(now);
	CRadar::SetEntityBlip(BLIP_CAR, CPools::GetVehiclePool()->GetIndex(car), 0, BLIP_DISPLAY_BOTH);

	s_shift.caseActive = true;
	s_shift.caseStartAt = now;
	s_shift.stoppedSince = 0;
	s_shift.closeRetargetAt = now;
	return true;
}

// The ped and the car go away together: RemoveThisPed handles the seat and
// the mission-ped count, and the car takes the DELETE_CAR door.  A car a player
// has climbed into during the cleanup delay -- the takedown leaves it standing
// there for them -- is theirs instead and goes back to the world like an
// escaped one: deleting it would delete them.
void
RemoveSuspect(void)
{
	CPed *ped = s_shift.suspectPed;
	CVehicle *car = s_shift.suspectCar;
	if(ped != nil){
		ped->CleanUpOldReference((CEntity**)&s_shift.suspectPed);
		s_shift.suspectPed = nil;
		CTheScripts::RemoveThisPed(ped);
	}
	if(car != nil){
		car->CleanUpOldReference((CEntity**)&s_shift.suspectCar);
		s_shift.suspectCar = nil;
		if(PlayerUsingVehicle(car)){
			CTheScripts::CleanUpThisVehicle(car);
			return;
		}
		CWorld::Remove(car);
		CWorld::RemoveReferencesToDeletedObject(car);
		delete car;
	}
}

// They got away: the world takes them back, the way a mission hands a car
// back to the traffic system.  The suspect simply becomes traffic again.
void
ReleaseSuspect(void)
{
	CPed *ped = s_shift.suspectPed;
	CVehicle *car = s_shift.suspectCar;
	if(ped != nil){
		CTheScripts::CleanUpThisPed(ped);
		ped->CleanUpOldReference((CEntity**)&s_shift.suspectPed);
		s_shift.suspectPed = nil;
	}
	if(car != nil){
		CTheScripts::CleanUpThisVehicle(car);
		car->CleanUpOldReference((CEntity**)&s_shift.suspectCar);
		s_shift.suspectCar = nil;
	}
}

void
RunCleanup(void)
{
	if(s_shift.escaped)
		ReleaseSuspect();
	else
		RemoveSuspect();
	s_shift.cleanupPending = false;
}

void
CloseCase(uint32 now, bool caught)
{
	if(caught){
		s_shift.streak++;
		const uint32 pay = 100 + 50 * (s_shift.streak - 1);
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += pay;
		s_shift.resultAt = now;
		s_shift.resultKind = SHIFT_RESULT_CAUGHT;
		s_shift.resultPay = pay;
		COOP_LOG("WII coop: case closed, streak %u\n", (unsigned)s_shift.streak);
	}else{
		s_shift.streak = 0;
		s_shift.resultAt = now;
		s_shift.resultKind = SHIFT_RESULT_AWAY;
		COOP_LOG("WII coop: case lost\n");
	}
	if(s_shift.suspectCar != nil)
		CRadar::ClearBlipForEntity(BLIP_CAR, CPools::GetVehiclePool()->GetIndex(s_shift.suspectCar));
	s_shift.caseActive = false;
	s_shift.stoppedSince = 0;
	s_shift.escaped = !caught;
	s_shift.cleanupPending = true;
	s_shift.cleanupAt = now + (caught ? kCleanupDelayMs : 0);
	s_shift.nextCaseAt = s_shift.cleanupAt + kCaseCooldownMs;
}

void
UpdateSpawnRequest(uint32 now)
{
	if(CStreaming::ms_aInfoForModel[s_shift.spawnCarModel].m_loadState != STREAMSTATE_LOADED ||
	   CStreaming::ms_aInfoForModel[s_shift.spawnPedModel].m_loadState != STREAMSTATE_LOADED){
		if(now - s_shift.spawnRequestAt > 4000){
			// Nothing came of it; try again later rather than stall the shift.
			s_shift.spawnPending = false;
			s_shift.nextCaseAt = now + kCaseCooldownMs;
		}
		return;
	}
	s_shift.spawnPending = false;
	if(!SpawnSuspect(now))
		s_shift.nextCaseAt = now + kCaseCooldownMs;
}

void
UpdateCase(uint32 now)
{
	if(s_shift.spawnPending){
		UpdateSpawnRequest(now);
		return;
	}

	CVehicle *car = s_shift.suspectCar;
	CPed *ped = s_shift.suspectPed;
	if(car == nil && ped == nil){
		CloseCase(now, false);
		return;
	}

	// A wreck, or a driver who is not getting up, closes it.
	if(car != nil && car->GetStatus() == STATUS_WRECKED){
		CloseCase(now, true);
		return;
	}
	if(ped != nil && ped->m_nPedState == PED_DEAD){
		CloseCase(now, true);
		return;
	}

	// Once the driver is out of the car the case is the runner.
	const bool onFoot = car == nil || (car->pDriver == nil && ped != nil);
	const CVector target = (onFoot && ped != nil) ? ped->GetPosition() : car->GetPosition();

	if(onFoot && ped->m_nPedState != PED_FLEE_ENTITY)
		ped->SetFlee(NearestPlayerPed(target), 60000);

	const float nearest = NearestPlayerDist(target);
	if(nearest > kCaseLeash || now - s_shift.caseStartAt > kCaseTimeoutMs){
		CloseCase(now, false);
		return;
	}

	// Cornered: held still with a player on top of it.
	const float speed = onFoot ? ped->m_vecMoveSpeed.Magnitude() : car->GetMoveSpeed().Magnitude();
	if(nearest < kStopRange && speed < kStopSpeed){
		if(s_shift.stoppedSince == 0)
			s_shift.stoppedSince = now;
		else if(now - s_shift.stoppedSince >= kStopHoldMs){
			CloseCase(now, true);
			return;
		}
	}else{
		s_shift.stoppedSince = 0;
	}

	// Keep it running away: on a clock, and sooner when the crew is close.
	if(!onFoot && (now >= s_shift.nextRetargetAt ||
	   (nearest < kCloseRange && now >= s_shift.closeRetargetAt))){
		DriveSuspectAway(now);
		s_shift.closeRetargetAt = now + kCloseRetargetMs;
	}
}

void
StartCase(uint32 now)
{
	s_shift.spawnCarModel = kCaseCars[CGeneral::GetRandomNumberInRange(0, ARRAY_SIZE(kCaseCars))];
	s_shift.spawnPedModel = MI_MALE01;
	CStreaming::RequestModel(s_shift.spawnCarModel, 0);
	CStreaming::RequestModel(s_shift.spawnPedModel, 0);
	s_shift.spawnPending = true;
	s_shift.spawnRequestAt = now;
	COOP_LOG("WII coop: case open\n");
}

void
EndShift(uint32 now, const char *why, int result)
{
	if(result != SHIFT_RESULT_NONE){
		s_shift.resultAt = now;
		s_shift.resultKind = result;
	}
	if(s_shift.caseActive){
		if(s_shift.suspectCar != nil)
			CRadar::ClearBlipForEntity(BLIP_CAR, CPools::GetVehiclePool()->GetIndex(s_shift.suspectCar));
		s_shift.caseActive = false;
		s_shift.escaped = true;
		s_shift.cleanupPending = true;
		s_shift.cleanupAt = now;
	}
	s_shift.spawnPending = false;
	s_shift.active = false;
	s_shift.streak = 0;
	s_shift.crewLostAt = 0;
	s_shift.stoppedSince = 0;
	COOP_LOG("WII coop: cop shift off (%s)\n", why);
}

void
TryStartShift(uint32 now)
{
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	// The law's view of the party: nobody starts a shift with a record.
	CPlayerPed *lead = FindPlayerPed();
	if(lead == nil || lead->m_pWanted == nil || lead->m_pWanted->GetWantedLevel() > 0)
		return;
	if(FindCrewedLawCar(true) == nil)
		return;

	s_shift.active = true;
	s_shift.streak = 0;
	s_shift.crewLostAt = 0;
	s_shift.resultAt = 0;
	s_shift.nextCaseAt = now + kShiftStartDelayMs;
	COOP_LOG("WII coop: cop shift on\n");
	CCoop::Notify("WII_CSH");
	ShowExplainer(EXPLAIN_SHIFT, now);
}

// --- the smuggling run -------------------------------------------------------

// A path node on water.  Boats live on the car network's water-flagged nodes,
// and asking for one is how every point below stays on navigable water.
bool
FindWaterNodeNear(const CVector &point, float radius, CVector &out)
{
	const int32 node = ThePaths.FindNodeClosestToCoors(point, PATH_CAR, radius, false, false, false, true);
	if(node < 0)
		return false;
	out = ThePaths.m_pathNodes[node].GetPosition();
	return true;
}

// The boat the party is riding in: a player driving, this many players or
// more aboard.  Only the arming uses this; the run itself holds its own boat.
CVehicle *
FindCrewBoat(int minPlayers)
{
	CVehicle *best = nil;
	int bestCount = 0;
	for(int player = 0; player < NUMPLAYERS; player++){
		CPlayerPed *ped = CCoop::GetPlayerPed(player);
		if(ped == nil || !ped->bInVehicle || ped->m_pMyVehicle == nil)
			continue;
		CVehicle *veh = ped->m_pMyVehicle;
		if(veh->GetVehicleAppearance() != VEHICLE_APPEARANCE_BOAT)
			continue;
		if(veh->pDriver == nil || !veh->pDriver->IsPlayer())
			continue;
		int count = 0;
		for(int p = 0; p < NUMPLAYERS; p++){
			CPlayerPed *other = CCoop::GetPlayerPed(p);
			if(other != nil && other->bInVehicle && other->m_pMyVehicle == veh)
				count++;
		}
		if(count >= minPlayers && count > bestCount){
			bestCount = count;
			best = veh;
		}
	}
	return best;
}

// The drop marker: a radar blip in the game's target red, and a corona over
// the water so the ring is visible from the boat.  The corona is registered
// every frame while a drop is live and fades on its own once it stops being.
// SetCoordBlip takes a colour and ignores it -- every blip it makes is magenta
// -- so the colour is set after.
void
ClearDropMarker(void)
{
	if(s_run.blip >= 0){
		CRadar::ClearBlip(s_run.blip);
		s_run.blip = -1;
	}
}

void
PlaceDropMarker(void)
{
	s_run.blip = CRadar::SetCoordBlip(BLIP_COORD, s_run.drop, RADAR_TRACE_RED, BLIP_DISPLAY_BOTH);
	CRadar::ChangeBlipColour(s_run.blip, RADAR_TRACE_RED);
}

void
RegisterDropCorona(void)
{
	CCoronas::RegisterCorona((uintptr)&s_run,
		255, 170, 70, 255,
		s_run.drop, 5.0f, 250.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
		CCoronas::FLARE_NONE, CCoronas::REFLECTION_ON,
		CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
}

// Three drops, each a leg on from the last and turning from the boat's own
// heading, each on water: the run follows the coast wherever it starts.  The
// nearest water node to a point three hundred metres out can be a node right
// beside where the leg started -- a narrow channel, a dock -- so a drop that
// lands within kMinDropSeparation of the boat or of an earlier drop is not
// taken, and another heading is tried.
bool
PlanRoute(CVehicle *boat)
{
	const CVector start = boat->GetPosition();
	CVector prev = start;
	CVector dir = boat->GetForward();
	dir.z = 0.0f;
	if(dir.Magnitude() < 0.1f)
		dir = CVector(1.0f, 0.0f, 0.0f);
	dir.Normalise();

	for(int leg = 0; leg < kDrops; leg++){
		bool found = false;
		for(int attempt = 0; attempt < 6 && !found; attempt++){
			const float turn = DEGTORAD((float)CGeneral::GetRandomNumberInRange(-70, 70));
			const float c = Cos(turn);
			const float s = Sin(turn);
			const CVector ndir(dir.x * c - dir.y * s, dir.x * s + dir.y * c, 0.0f);
			if(!FindWaterNodeNear(prev + ndir * kLegDistance, kDropSearch, s_run.dropPos[leg]))
				continue;
			bool tooClose = (s_run.dropPos[leg] - start).Magnitude2D() < kMinDropSeparation;
			for(int earlier = 0; earlier < leg && !tooClose; earlier++)
				tooClose = (s_run.dropPos[leg] - s_run.dropPos[earlier]).Magnitude2D() < kMinDropSeparation;
			if(!tooClose){
				dir = ndir;
				found = true;
			}
		}
		if(!found)
			return false;
		prev = s_run.dropPos[leg];
	}
	return true;
}

// The heat goes the way the suspect does: the world takes the boats back when
// the run is over, so a Predator drifts off as another boat on the water.
void
ReleasePursuers(void)
{
	for(int i = 0; i < 2; i++){
		CPed *cop = s_run.pursuerDriver[i];
		CVehicle *boat = s_run.pursuer[i];
		if(cop != nil){
			CTheScripts::CleanUpThisPed(cop);
			cop->CleanUpOldReference((CEntity**)&s_run.pursuerDriver[i]);
			s_run.pursuerDriver[i] = nil;
		}
		if(boat != nil){
			CTheScripts::CleanUpThisVehicle(boat);
			boat->CleanUpOldReference((CEntity**)&s_run.pursuer[i]);
			s_run.pursuer[i] = nil;
		}
	}
}

// Chase the smuggling boat: COMMAND_BOAT_GOTO_COORDS' recipe, on a water node
// beside the boat, re-aimed on a clock.  Not the car recipe: the road-system
// call searches the car network without ever looking at the vehicle it was
// given, so it would route a boat along roads; the boat mission steers
// straight at the destination, over water.
void
DrivePursuer(int index, uint32 now, CVehicle *boat)
{
	CVehicle *predator = s_run.pursuer[index];
	if(predator == nil || boat == nil)
		return;
	CVector dest;
	if(!FindWaterNodeNear(boat->GetPosition(), 150.0f, dest))
		dest = boat->GetPosition();
	predator->AutoPilot.m_vecDestinationCoors = dest;
	predator->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS_ASTHECROWSWIMS;
	predator->SetStatus(STATUS_PHYSICS);
	predator->bEngineOn = true;
	predator->AutoPilot.m_nCruiseSpeed = predator->AutoPilot.m_fMaxTrafficSpeed = 25;
	predator->AutoPilot.m_nAntiReverseTimer = now;
}

// One police boat's worth of heat: the Predator is the game's own police
// boat, a cop drives it, and both halves are requested when the run arms so
// the first drop finds them in memory.
void
SpawnPursuer(uint32 now, CVehicle *boat, int index)
{
	if(s_run.pursuer[index] != nil || boat == nil)
		return;
	if(!CStreaming::HasModelLoaded(MI_PREDATOR) || !CStreaming::HasModelLoaded(MI_COP))
		return;

	CVector spawnPos;
	bool found = false;
	for(int attempt = 0; attempt < 4 && !found; attempt++){
		const float angle = DEGTORAD((float)CGeneral::GetRandomNumberInRange(0, 360));
		const CVector dir(Cos(angle), Sin(angle), 0.0f);
		found = FindWaterNodeNear(boat->GetPosition() + dir * kPursuerDistance, 200.0f, spawnPos);
	}
	if(!found)
		return;

	CBoat *predator = new CBoat(MI_PREDATOR, MISSION_VEHICLE);
	if(predator == nil)
		return;
	spawnPos.z += predator->GetDistanceFromCentreOfMassToBaseOfModel();
	predator->SetPosition(spawnPos);
	CTheScripts::ClearSpaceForMissionEntity(spawnPos, predator);
	predator->SetStatus(STATUS_ABANDONED);
	predator->bIsLocked = true;
	predator->AutoPilot.m_nCarMission = MISSION_NONE;
	predator->AutoPilot.m_nTempAction = TEMPACT_NONE;
	predator->AutoPilot.m_nCruiseSpeed = predator->AutoPilot.m_fMaxTrafficSpeed = 25;
	predator->bEngineOn = true;
	predator->m_nZoneLevel = CTheZones::GetLevelFromPosition(&spawnPos);
	CWorld::Add(predator);

	CCopPed *cop = new CCopPed(COP_STREET);
	if(cop == nil){
		CWorld::Remove(predator);
		CWorld::RemoveReferencesToDeletedObject(predator);
		delete predator;
		return;
	}
	cop->CharCreatedBy = MISSION_CHAR;
	cop->bRespondsToThreats = false;
	cop->bAllowMedicsToReviveMe = false;
	cop->bIsPlayerFriend = false;
	CVector copPos = predator->GetPosition();
	copPos.z += 1.0f;
	cop->SetPosition(copPos);
	cop->SetOrientation(0.0f, 0.0f, 0.0f);
	CWorld::Add(cop);
	cop->m_nZoneLevel = CTheZones::GetLevelFromPosition(&copPos);
	++CPopulation::ms_nTotalMissionPeds;
	cop->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, predator);
	cop->WarpPedIntoCar(predator);

	predator->RegisterReference((CEntity**)&s_run.pursuer[index]);
	cop->RegisterReference((CEntity**)&s_run.pursuerDriver[index]);
	s_run.pursuer[index] = predator;
	s_run.pursuerDriver[index] = cop;
	DrivePursuer(index, now, boat);
	COOP_LOG("WII coop: heat on the water\n");
}

void
UpdatePursuers(uint32 now, CVehicle *boat)
{
	if(now < s_run.nextRetargetAt)
		return;
	s_run.nextRetargetAt = now + kPursuerRetargetMs;
	if(boat == nil)
		return;
	for(int i = 0; i < 2; i++)
		DrivePursuer(i, now, boat);
}

void
TryStartRun(uint32 now)
{
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	if(now < s_run.rearmAt)
		return;
	CVehicle *boat = FindCrewBoat(2);
	if(boat == nil)
		return;
	CPad *pad = GetPadFromVehicleDriver(boat);
	if(pad == nil || !pad->GetHorn())
		return;
	if(!PlanRoute(boat))
		return;

	CStreaming::RequestModel(MI_PREDATOR, 0);
	CStreaming::RequestModel(MI_COP, 0);

	boat->RegisterReference((CEntity**)&s_run.boat);
	s_run.boat = boat;
	s_run.active = true;
	s_run.leg = 0;
	s_run.drop = s_run.dropPos[0];
	s_run.unloadSince = 0;
	s_run.crewLostAt = 0;
	s_run.nextRetargetAt = now;
	PlaceDropMarker();
	COOP_LOG("WII coop: smuggling run on\n");
	CCoop::Notify("WII_CSR");
	ShowExplainer(EXPLAIN_RUN, now);
}

void
EndRun(uint32 now, const char *why)
{
	ClearDropMarker();
	ReleasePursuers();
	if(s_run.boat != nil){
		s_run.boat->CleanUpOldReference((CEntity**)&s_run.boat);
		s_run.boat = nil;
	}
	s_run.active = false;
	s_run.rearmAt = now + kRearmMs;
	s_run.unloadSince = 0;
	s_run.crewLostAt = 0;
	COOP_LOG("WII coop: smuggling run off (%s)\n", why);
}

void
CompleteDrop(uint32 now, CVehicle *boat)
{
	CWorld::Players[CWorld::PlayerInFocus].m_nMoney += 100;
	CPlayerPed *lead = FindPlayerPed();
	if(lead != nil && lead->m_pWanted != nil && lead->m_pWanted->GetWantedLevel() < 3)
		lead->m_pWanted->SetWantedLevel(lead->m_pWanted->GetWantedLevel() + 1);

	s_run.leg++;
	COOP_LOG("WII coop: drop %d of %d\n", s_run.leg, kDrops);
	if(s_run.leg >= kDrops){
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += 300;
		if(lead != nil){
			if(lead->DoesPlayerWantNewWeapon(WEAPONTYPE_UZI, true))
				lead->GiveWeapon(WEAPONTYPE_UZI, 120, true);
			else
				lead->GrantAmmo(WEAPONTYPE_UZI, 120);
		}
		EndRun(now, "delivered");
		return;
	}

	ClearDropMarker();
	s_run.drop = s_run.dropPos[s_run.leg];
	s_run.unloadSince = 0;
	PlaceDropMarker();
	// The heat steps up with every drop: one police boat, then its partner.
	SpawnPursuer(now, boat, s_run.leg - 1);
}

void
UpdateRun(uint32 now)
{
	if(!s_run.active){
		TryStartRun(now);
		return;
	}
	if(!CCoop::PairActive()){
		EndRun(now, "session");
		return;
	}

	// The boat is the run: while it exists and somebody is aboard it goes on;
	// lost, wrecked, or empty for a moment and it is over.
	CVehicle *boat = s_run.boat;
	if(boat == nil){
		EndRun(now, "boat lost");
		return;
	}
	if(boat->GetStatus() == STATUS_WRECKED){
		EndRun(now, "boat wrecked");
		return;
	}
	int crew = 0;
	for(int player = 0; player < NUMPLAYERS; player++){
		CPlayerPed *ped = CCoop::GetPlayerPed(player);
		if(ped != nil && ped->bInVehicle && ped->m_pMyVehicle == boat)
			crew++;
	}
	if(crew == 0){
		if(s_run.crewLostAt == 0)
			s_run.crewLostAt = now;
		else if(now - s_run.crewLostAt >= kCrewGraceMs)
			EndRun(now, "crew ashore");
		// A halt while nobody is aboard does not keep counting towards an
		// unload.
		s_run.unloadSince = 0;
		UpdatePursuers(now, boat);
		return;
	}
	s_run.crewLostAt = 0;

	// Hold the boat in the ring, slow, and it unloads.
	const float dist = (boat->GetPosition() - s_run.drop).Magnitude2D();
	const float speed = boat->GetMoveSpeed().Magnitude();
	if(dist < kDropRadius && speed < kDropSpeed){
		if(s_run.unloadSince == 0)
			s_run.unloadSince = now;
		else if(now - s_run.unloadSince >= kUnloadMs){
			CompleteDrop(now, boat);
			return;
		}
	}else{
		s_run.unloadSince = 0;
	}

	UpdatePursuers(now, boat);
	RegisterDropCorona();
}

// --- the Bloodring derby -----------------------------------------------------

// The derby is ramming only: the fire button reads as off for the duration,
// the same way the one-operator rule keeps a driver from firing under a
// gunner.  The button is the one CPad::GetWeapon reads for this pad's control
// mode, and nothing else -- on the Wii's own layout that is B, while A keeps
// accelerating.  Cleared after CPad::UpdatePads, before the world processes.
void
SuppressFire(CPad *pad)
{
	const int mode = CPad::IsAffectedByController ? pad->Mode : 0;
	switch(mode){
	case 2: pad->NewState.Cross = false; break;
	case 3: pad->NewState.RightShoulder1 = false; break;
	default: pad->NewState.Circle = false; break;
	}
}

// Lets go of a player's loaner: the world takes it back, like the suspect and
// the Predators.
void
ReleaseLoaner(int player)
{
	CVehicle *car = s_derby.loaner[player];
	if(car != nil){
		CTheScripts::CleanUpThisVehicle(car);
		car->CleanUpOldReference((CEntity**)&s_derby.loaner[player]);
		s_derby.loaner[player] = nil;
	}
}

// A Bloodring Banger beside a player who needs one, parked, locked, ready.
// Handed back to the world when the derby is over.  A loaner that was wrecked
// in an earlier round is let go and replaced: this is only asked of a player
// with no running car, and a wreck standing there would leave them without.
void
SpawnLoaner(int player)
{
	if(!CStreaming::HasModelLoaded(MI_BLOODRA))
		return;
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	if(ped == nil)
		return;
	if(s_derby.loaner[player] != nil && s_derby.loaner[player]->GetStatus() == STATUS_WRECKED)
		ReleaseLoaner(player);
	if(s_derby.loaner[player] != nil)
		return;

	CVector pos = ped->GetPosition() + ped->GetRight()*4.0f;
	bool found = false;
	const float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
	pos.z = found ? ground : ped->GetPosition().z;

	CAutomobile *car = new CAutomobile(MI_BLOODRA, MISSION_VEHICLE);
	if(car == nil)
		return;
	pos.z += car->GetDistanceFromCentreOfMassToBaseOfModel();
	car->SetPosition(pos);
	car->SetOrientation(0.0f, 0.0f, 0.0f);
	car->SetStatus(STATUS_ABANDONED);
	car->bIsLocked = true;
	car->AutoPilot.m_nCarMission = MISSION_NONE;
	car->AutoPilot.m_nTempAction = TEMPACT_NONE;
	car->bEngineOn = true;
	car->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	CWorld::Add(car);
	car->RegisterReference((CEntity**)&s_derby.loaner[player]);
	s_derby.loaner[player] = car;
}

bool
PlayerInDerbyCar(int player, CVehicle **out)
{
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	if(ped == nil || !ped->bInVehicle || ped->m_pMyVehicle == nil)
		return false;
	CVehicle *veh = ped->m_pMyVehicle;
	if(!veh->IsCar() || veh->bIsLawEnforcer || veh->pDriver != ped)
		return false;
	*out = veh;
	return true;
}

void
StartDerby(uint32 now, const CVector &centre)
{
	s_derby.active = true;
	s_derby.counting = true;
	s_derby.finished = false;
	s_derby.countdownAt = now + kDerbyCountdownMs;
	s_derby.centre = centre;
	// The ring, once: where it stands on the ground is a collision query, so
	// it is worked out here and not every frame it is drawn.
	for(int i = 0; i < kDerbyRingMarks; i++){
		const float angle = 2.0f*PI*(float)i/(float)kDerbyRingMarks;
		CVector mark = centre + CVector(Cos(angle), Sin(angle), 0.0f)*kDerbyRadius;
		bool found = false;
		const float ground = CWorld::FindGroundZFor3DCoord(mark.x, mark.y, mark.z + 2.0f, &found);
		mark.z = (found ? ground : centre.z) + 1.0f;
		s_derby.ring[i] = mark;
	}
	s_derby.round = 0;
	s_derby.roundStartAt = 0;
	s_derby.matchWinner = -1;
	for(int i = 0; i < NUMPLAYERS; i++){
		s_derby.roundWins[i] = 0;
		s_derby.running[i] = false;
		s_derby.outSince[i] = 0;
		s_derby.footSince[i] = 0;
	}
	CStreaming::RequestModel(MI_BLOODRA, 0);
	COOP_LOG("WII coop: derby on\n");
	CCoop::Notify("WII_CSD");
	ShowExplainer(EXPLAIN_DERBY, now);
}

// Whoever has the most rounds takes the match, if anyone has any.
void
FinishDerby(uint32 now)
{
	int best = -1;
	int bestWins = 0;
	for(int i = 0; i < NUMPLAYERS; i++)
		if(s_derby.roundWins[i] > bestWins){
			bestWins = s_derby.roundWins[i];
			best = i;
		}
	if(best >= 0){
		s_derby.matchWinner = best;
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += 500;
		COOP_LOG("WII coop: derby to player %d\n", best + 1);
	}
	s_derby.finished = true;
	s_derby.finishAt = now + 4000;
}

void
StartDerbyRound(uint32 now)
{
	s_derby.round++;
	s_derby.counting = false;
	s_derby.roundStartAt = now;
	for(int i = 0; i < NUMPLAYERS; i++){
		s_derby.running[i] = false;
		s_derby.outSince[i] = 0;
		s_derby.footSince[i] = 0;
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil || !ped->bInVehicle || ped->m_pMyVehicle == nil)
			continue;
		CVehicle *veh = ped->m_pMyVehicle;
		if(!veh->IsCar() || veh->bIsLawEnforcer || veh->GetStatus() == STATUS_WRECKED)
			continue;
		if((veh->GetPosition() - s_derby.centre).Magnitude2D() > kDerbyJoinRange)
			continue;
		s_derby.running[i] = true;
	}

	int running = 0;
	for(int i = 0; i < NUMPLAYERS; i++)
		if(s_derby.running[i])
			running++;
	if(running < 2){
		// Nobody to race; the match goes to whoever has wins.
		FinishDerby(now);
		return;
	}
	COOP_LOG("WII coop: derby round %d\n", s_derby.round);
}

void
EndDerby(uint32 now, const char *why)
{
	for(int i = 0; i < NUMPLAYERS; i++)
		ReleaseLoaner(i);
	s_derby.active = false;
	s_derby.counting = false;
	s_derby.finished = false;
	s_derby.rearmAt = now + kDerbyRearmMs;
	COOP_LOG("WII coop: derby off (%s)\n", why);
}

void
TryStartDerby(uint32 now)
{
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	if(now < s_derby.rearmAt)
		return;

	// The party, parked in a knot: two or more of their own cars, all stopped,
	// all within arm's length of the middle.
	CVehicle *car[NUMPLAYERS];
	CVector sum(0.0f, 0.0f, 0.0f);
	int count = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		car[i] = nil;
		if(!PlayerInDerbyCar(i, &car[i]))
			continue;
		sum += car[i]->GetPosition();
		count++;
	}
	if(count < 2)
		return;
	const CVector centre = sum / (float)count;
	for(int i = 0; i < NUMPLAYERS; i++){
		if(car[i] == nil)
			continue;
		if(car[i]->GetMoveSpeed().Magnitude() > kDerbyParkedSpeed ||
		   (car[i]->GetPosition() - centre).Magnitude2D() > kDerbyGatherRange)
			return;
	}

	// A honk from any of them is the go-ahead.
	for(int i = 0; i < NUMPLAYERS; i++){
		if(car[i] == nil)
			continue;
		CPad *pad = GetPadFromVehicleDriver(car[i]);
		if(pad != nil && pad->GetHorn()){
			StartDerby(now, centre);
			return;
		}
	}
}

// The ring itself: a circle of red coronas on the ground round the middle,
// registered every frame it stands, from the countdown to the last bell.  It
// is the edge the round is judged by -- three seconds outside it and a player
// is out -- so it has to be seen.
void
RegisterDerbyCoronas(void)
{
	for(int i = 0; i < kDerbyRingMarks; i++)
		CCoronas::RegisterCorona((uintptr)&s_derby + i, 255, 60, 40, 255,
			s_derby.ring[i], 3.0f, 150.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
			CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
			CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
}

void
UpdateDerby(uint32 now)
{
	if(!s_derby.active){
		TryStartDerby(now);
		return;
	}
	if(!CCoop::PairActive()){
		EndDerby(now, "session");
		return;
	}
	if(s_derby.finished){
		if(now >= s_derby.finishAt)
			EndDerby(now, "over");
		return;
	}
	RegisterDerbyCoronas();

	// Ramming only, for everyone, for the whole match.
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil)
			SuppressFire(GetPadFromPlayer(ped));
	}

	if(s_derby.counting){
		if(now >= s_derby.countdownAt)
			StartDerbyRound(now);
		return;
	}

	// The round: wrecked, out of the ring for a moment, or out of the car for
	// a moment, and a player is done.
	int alive = 0;
	int survivor = -1;
	for(int i = 0; i < NUMPLAYERS; i++){
		if(!s_derby.running[i])
			continue;
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil){
			s_derby.running[i] = false;
			continue;
		}
		CVehicle *veh = ped->bInVehicle ? ped->m_pMyVehicle : nil;
		if(veh != nil && veh->GetStatus() == STATUS_WRECKED){
			s_derby.running[i] = false;
			continue;
		}
		const CVector pos = veh != nil ? veh->GetPosition() : ped->GetPosition();
		if((pos - s_derby.centre).Magnitude2D() > kDerbyRadius){
			if(s_derby.outSince[i] == 0)
				s_derby.outSince[i] = now;
			else if(now - s_derby.outSince[i] >= kDerbyOutGraceMs){
				s_derby.running[i] = false;
				continue;
			}
		}else{
			s_derby.outSince[i] = 0;
		}
		if(veh == nil){
			if(s_derby.footSince[i] == 0)
				s_derby.footSince[i] = now;
			else if(now - s_derby.footSince[i] >= kDerbyFootGraceMs){
				s_derby.running[i] = false;
				continue;
			}
		}else{
			s_derby.footSince[i] = 0;
		}
		alive++;
		survivor = i;
	}

	if(alive > 1 && now - s_derby.roundStartAt <= kDerbyRoundCapMs)
		return;

	// The round is over: the survivor takes it, a round that ran out of time
	// takes nobody.
	if(alive == 1){
		s_derby.roundWins[survivor]++;
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += 150;
		COOP_LOG("WII coop: derby round to player %d\n", survivor + 1);
	}

	int bestWins = 0;
	for(int i = 0; i < NUMPLAYERS; i++)
		if(s_derby.roundWins[i] > bestWins)
			bestWins = s_derby.roundWins[i];
	if(bestWins >= kDerbyWinsNeeded){
		FinishDerby(now);
		return;
	}

	// Intermission: a Banger for anyone whose car is gone, then another round.
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		CVehicle *veh = ped->bInVehicle ? ped->m_pMyVehicle : nil;
		if((veh == nil || veh->GetStatus() == STATUS_WRECKED) &&
		   (ped->GetPosition() - s_derby.centre).Magnitude2D() <= kDerbyJoinRange + 20.0f)
			SpawnLoaner(i);
	}
	s_derby.counting = true;
	s_derby.countdownAt = now + kDerbyIntermissionMs;
}

// --- the RC race -------------------------------------------------------------

bool
PlayerInRcCar(int player, CVehicle **out)
{
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	if(ped == nil || !ped->bInVehicle || ped->m_pMyVehicle == nil)
		return false;
	for(int i = 0; i < NUMPLAYERS; i++)
		if(s_rc.car[i] == ped->m_pMyVehicle){
			*out = ped->m_pMyVehicle;
			return true;
		}
	return false;
}

// A normal-sized ped in a toy car is a normal-sized ped through a toy car's
// roof; the car is the thing being driven, so the driver is not drawn.  The
// engine puts them back when they get out.
void
SuppressRcBody(CPlayerPed *ped)
{
	ped->bRenderPedInCar = false;
}

void
ReleaseRcCars(void)
{
	for(int i = 0; i < NUMPLAYERS; i++){
		CVehicle *car = s_rc.car[i];
		if(car != nil){
			CTheScripts::CleanUpThisVehicle(car);
			car->CleanUpOldReference((CEntity**)&s_rc.car[i]);
			s_rc.car[i] = nil;
		}
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil)
			ped->bRenderPedInCar = true;
	}
}

void
EndRc(uint32 now, const char *why)
{
	ReleaseRcCars();
	s_rc.active = false;
	s_rc.grid = false;
	s_rc.counting = false;
	s_rc.finished = false;
	s_rc.rearmAt = now + kRcRearmMs;
	COOP_LOG("WII coop: rc race off (%s)\n", why);
}

bool
SpawnRcCar(int player, const CVector &slot, const CVector &forward)
{
	if(s_rc.car[player] != nil)
		return true;
	if(!CStreaming::HasModelLoaded(MI_RCBANDIT))
		return false;
	CVector pos = slot;
	bool found = false;
	const float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
	if(found)
		pos.z = ground;
	CAutomobile *car = new CAutomobile(MI_RCBANDIT, MISSION_VEHICLE);
	if(car == nil)
		return false;
	pos.z += car->GetDistanceFromCentreOfMassToBaseOfModel();
	car->SetPosition(pos);
	car->GetForward() = forward;
	car->GetRight() = CVector(forward.y, -forward.x, 0.0f);
	car->GetUp() = CVector(0.0f, 0.0f, 1.0f);
	car->SetStatus(STATUS_ABANDONED);
	car->bIsLocked = true;
	car->AutoPilot.m_nCarMission = MISSION_NONE;
	car->AutoPilot.m_nTempAction = TEMPACT_NONE;
	car->bEngineOn = true;
	car->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	CWorld::Add(car);
	car->RegisterReference((CEntity**)&s_rc.car[player]);
	s_rc.car[player] = car;
	return true;
}

void
RcSlot(int player, CVector &out)
{
	const CVector right(s_rc.forward.y, -s_rc.forward.x, 0.0f);
	out = s_rc.centre + right*((player - 1.5f) * 5.0f);
}

// A wrecked or lost toy is replaced beside the driver, once they are on their
// feet: the engine does the leaving itself, so there is no half-in, half-out
// ped to untangle, and the laps already driven stay.  The driver walks into
// the replacement like any other car.
void
RespawnRcCar(int player)
{
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	if(ped == nil)
		return;
	CVehicle *old = s_rc.car[player];
	if(old != nil){
		CTheScripts::CleanUpThisVehicle(old);
		old->CleanUpOldReference((CEntity**)&s_rc.car[player]);
		s_rc.car[player] = nil;
	}
	SpawnRcCar(player, ped->GetPosition() + ped->GetRight()*4.0f, s_rc.forward);
}

void
StartRc(uint32 now, const CVector &centre, const CVector &forward)
{
	s_rc.active = true;
	s_rc.grid = true;
	s_rc.counting = false;
	s_rc.finished = false;
	s_rc.gridAt = now;
	s_rc.gridReadyAt = 0;
	s_rc.winner = -1;
	s_rc.centre = centre;
	s_rc.forward = forward;

	// One gate ahead, then round the ring at right angles.
	const CVector right(forward.y, -forward.x, 0.0f);
	const CVector dirs[kRcGates] = { forward, right, forward*-1.0f, right*-1.0f };
	for(int i = 0; i < kRcGates; i++){
		CVector gate = centre + dirs[i]*kRcRadius;
		bool found = false;
		const float ground = CWorld::FindGroundZFor3DCoord(gate.x, gate.y, gate.z + 2.0f, &found);
		gate.z = (found ? ground : centre.z) + 1.0f;
		s_rc.gate[i] = gate;
	}
	for(int i = 0; i < NUMPLAYERS; i++){
		s_rc.nextGate[i] = -1;
		s_rc.lap[i] = 0;
		s_rc.footSince[i] = 0;
	}
	CStreaming::RequestModel(MI_RCBANDIT, 0);
	COOP_LOG("WII coop: rc race armed\n");
	CCoop::Notify("WII_CSC");
	ShowExplainer(EXPLAIN_RC, now);
}

void
TryStartRc(uint32 now)
{
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	if(now < s_rc.rearmAt)
		return;

	// The party on foot, in a knot, and two or more of them jump together:
	// one person hopping about is not a race.
	CVector sum(0.0f, 0.0f, 0.0f);
	CVector forward(0.0f, 0.0f, 0.0f);
	int count = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(ped->bInVehicle)
			return;
		sum += ped->GetPosition();
		forward += ped->GetForward();
		count++;
		if(GetPadFromPlayer(ped)->JumpJustDown())
			s_rc.lastJump[i] = now;
	}
	if(count < 2)
		return;
	int jumped = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil && now - s_rc.lastJump[i] <= kRcJumpWindowMs)
			jumped++;
	}
	if(jumped < 2)
		return;
	const CVector centre = sum / (float)count;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil && (ped->GetPosition() - centre).Magnitude2D() > kRcGatherRange)
			return;
	}
	CVector dir = forward;
	dir.z = 0.0f;
	if(dir.Magnitude() < 0.1f)
		dir = CVector(1.0f, 0.0f, 0.0f);
	dir.Normalise();
	StartRc(now, centre, dir);
}

// The gates glow for everyone, and each driver's own next gate glows in their
// own colour: four gates in a ring only mean a race if you know which is
// yours.
void
RegisterRcCoronas(void)
{
	for(int i = 0; i < kRcGates; i++)
		CCoronas::RegisterCorona((uintptr)&s_rc + i, 120, 200, 255, 255,
			s_rc.gate[i], 4.0f, 200.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
			CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
			CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
	for(int i = 0; i < NUMPLAYERS; i++){
		if(s_rc.nextGate[i] < 0)
			continue;
		const CRGBA &colour = kCoopColours[i];
		CCoronas::RegisterCorona((uintptr)&s_rc + kRcGates + i, colour.r, colour.g, colour.b, 255,
			s_rc.gate[s_rc.nextGate[i]], 2.5f, 150.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
			CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
			CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
	}
}

void
UpdateRc(uint32 now)
{
	if(!s_rc.active){
		TryStartRc(now);
		return;
	}
	if(!CCoop::PairActive()){
		EndRc(now, "session");
		return;
	}
	if(s_rc.finished){
		if(now >= s_rc.finishAt)
			EndRc(now, "over");
		return;
	}

	if(s_rc.grid){
		// The grid fills as the model arrives; the lights come down once two
		// or more are seated and still.
		int seated = 0;
		bool moving = false;
		for(int i = 0; i < NUMPLAYERS; i++){
			CPlayerPed *ped = CCoop::GetPlayerPed(i);
			if(ped == nil)
				continue;
			if(s_rc.car[i] == nil){
				CVector slot;
				RcSlot(i, slot);
				SpawnRcCar(i, slot, s_rc.forward);
			}
			CVehicle *car;
			if(PlayerInRcCar(i, &car)){
				seated++;
				if(car->GetMoveSpeed().Magnitude() > kRcSeatedSpeed)
					moving = true;
				SuppressRcBody(ped);
			}
		}
		if(seated >= 2 && !moving){
			if(s_rc.gridReadyAt == 0)
				s_rc.gridReadyAt = now;
			else if(now - s_rc.gridReadyAt >= kRcGridHoldMs){
				s_rc.grid = false;
				s_rc.counting = true;
				s_rc.countdownAt = now + kRcCountdownMs;
			}
		}else{
			s_rc.gridReadyAt = 0;
		}
		if(now - s_rc.gridAt > kRcGridTimeoutMs)
			EndRc(now, "no grid");
		return;
	}

	if(s_rc.counting){
		if(now < s_rc.countdownAt)
			return;
		s_rc.counting = false;
		int racers = 0;
		for(int i = 0; i < NUMPLAYERS; i++){
			CVehicle *car;
			if(PlayerInRcCar(i, &car)){
				s_rc.nextGate[i] = 0;
				s_rc.lap[i] = 0;
				racers++;
			}else{
				s_rc.nextGate[i] = -1;
			}
		}
		if(racers < 2){
			EndRc(now, "no grid");
			return;
		}
		COOP_LOG("WII coop: rc race on\n");
		return;
	}

	// The race: gates in order, three laps, first across takes it.
	for(int i = 0; i < NUMPLAYERS; i++){
		if(s_rc.nextGate[i] < 0)
			continue;
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil){
			s_rc.nextGate[i] = -1;
			continue;
		}
		CVehicle *car = ped->bInVehicle ? ped->m_pMyVehicle : nil;
		if(car != nil && car->GetStatus() == STATUS_WRECKED)
			continue;	// the driver gets out first; the replacement waits
		if(car == nil){
			// On foot.  A toy is put beside the racer once -- at once if theirs
			// is gone or wrecked, after a few seconds of walking if it is not --
			// and then they walk into it.  Doing this every frame released the
			// last toy and raised a new one, dozens a second, until the racer
			// sat in one.  One on its way into a toy is not asking for another.
			if(ped->m_nPedState == PED_SEEK_CAR || ped->EnteringCar()){
				s_rc.footSince[i] = now;
				continue;
			}
			CVehicle *toy = s_rc.car[i];
			const bool first = s_rc.footSince[i] == 0;
			if(first)
				s_rc.footSince[i] = now;
			const bool gone = toy == nil || toy->GetStatus() == STATUS_WRECKED;
			if((first && gone) || now - s_rc.footSince[i] >= kRcFootGraceMs){
				s_rc.footSince[i] = now;
				RespawnRcCar(i);
			}
			continue;
		}
		s_rc.footSince[i] = 0;
		SuppressRcBody(ped);
		if((car->GetPosition() - s_rc.gate[s_rc.nextGate[i]]).Magnitude2D() < kRcGateRange){
			s_rc.nextGate[i]++;
			if(s_rc.nextGate[i] >= kRcGates){
				s_rc.nextGate[i] = 0;
				s_rc.lap[i]++;
				if(s_rc.lap[i] >= kRcLaps){
					s_rc.winner = i;
					CWorld::Players[CWorld::PlayerInFocus].m_nMoney += kRcPot;
					s_rc.finished = true;
					s_rc.finishAt = now + kRcLingerMs;
					COOP_LOG("WII coop: rc race to player %d\n", i + 1);
					return;
				}
			}
		}
	}
	RegisterRcCoronas();
}

// --- the dance-off -----------------------------------------------------------

uint32
DanceBeatInterval(uint32 now)
{
	float progress = (float)(now - s_dance.startAt) / (float)kDanceRoundMs;
	progress = Clamp(progress, 0.0f, 1.0f);
	// In float: the beat gets shorter, so fast minus slow is negative, and as
	// the unsigned milliseconds the constants are it wraps to four billion --
	// one beat, then nothing for the rest of the round.
	const float slow = (float)kDanceBeatSlowMs;
	const float fast = (float)kDanceBeatFastMs;
	return (uint32)(slow + (fast - slow)*progress);
}

// The stage: a ring in the four players' colours, the same colours the
// prompts come up in.
void
RegisterDanceCoronas(void)
{
	const CVector offsets[4] = {
		CVector(kDanceFloor, 0.0f, 0.0f), CVector(0.0f, kDanceFloor, 0.0f),
		CVector(-kDanceFloor, 0.0f, 0.0f), CVector(0.0f, -kDanceFloor, 0.0f)
	};
	for(int i = 0; i < 4; i++){
		CVector pos = s_dance.centre + offsets[i];
		pos.z += 1.0f;
		CCoronas::RegisterCorona((uintptr)&s_dance + i,
			kCoopColours[i].r, kCoopColours[i].g, kCoopColours[i].b, 255,
			pos, 3.5f, 150.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
			CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
			CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
	}
}

void
EndDance(uint32 now, const char *why)
{
	for(int i = 0; i < NUMPLAYERS; i++){
		s_dance.prompt[i] = -1;
		s_dance.promptHit[i] = false;
	}
	s_dance.active = false;
	s_dance.finished = false;
	s_dance.rearmAt = now + kDanceRearmMs;
	COOP_LOG("WII coop: dance-off off (%s)\n", why);
}

void
FinishDance(uint32 now)
{
	int totalHits = 0;
	int best = -1;
	int bestScore = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		totalHits += s_dance.score[i];
		if(s_dance.score[i] > bestScore){
			bestScore = s_dance.score[i];
			best = i;
		}
	}
	s_dance.winner = best;
	CWorld::Players[CWorld::PlayerInFocus].m_nMoney += kDanceHitPay*totalHits;
	COOP_LOG("WII coop: dance-off, %d hits\n", totalHits);
	if(best >= 0){
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += kDanceWinPot;
		COOP_LOG("WII coop: dance-off to player %d\n", best + 1);
	}
	s_dance.finished = true;
	s_dance.finishAt = now + kDanceLingerMs;
}

void
StartDance(uint32 now, const CVector &centre)
{
	s_dance.active = true;
	s_dance.finished = false;
	s_dance.startAt = now;
	s_dance.beatAt = now + kDanceBeatSlowMs;
	s_dance.centre = centre;
	s_dance.winner = -1;
	for(int i = 0; i < NUMPLAYERS; i++){
		s_dance.prompt[i] = -1;
		s_dance.promptHit[i] = false;
		s_dance.hitFlash[i] = 0;
		s_dance.missFlash[i] = 0;
		s_dance.score[i] = 0;
		s_dance.combo[i] = 0;
	}
	COOP_LOG("WII coop: dance-off on\n");
	CCoop::Notify("WII_CSF");
	ShowExplainer(EXPLAIN_DANCE, now);
}

void
TryStartDance(uint32 now)
{
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	if(now < s_dance.rearmAt)
		return;

	// The party on foot, in a knot, and every one of them ducking together:
	// the whole floor gets down, so the whole floor starts it.
	CVector sum(0.0f, 0.0f, 0.0f);
	int count = 0;
	int ducked = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(ped->bInVehicle)
			return;
		sum += ped->GetPosition();
		count++;
		if(GetPadFromPlayer(ped)->DuckJustDown())
			s_dance.lastDuck[i] = now;
		if(now - s_dance.lastDuck[i] <= kDanceGestureWindowMs)
			ducked++;
	}
	if(count < 2 || ducked < count)
		return;
	const CVector centre = sum / (float)count;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil && (ped->GetPosition() - centre).Magnitude2D() > kDanceGatherRange)
			return;
	}
	StartDance(now, centre);
}

void
UpdateDance(uint32 now)
{
	if(!s_dance.active){
		TryStartDance(now);
		return;
	}
	if(!CCoop::PairActive()){
		EndDance(now, "session");
		return;
	}
	if(s_dance.finished){
		if(now >= s_dance.finishAt)
			EndDance(now, "over");
		return;
	}
	if(now - s_dance.startAt >= kDanceRoundMs){
		FinishDance(now);
		return;
	}

	// On the floor: on foot, near the middle.
	bool onFloor[NUMPLAYERS];
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		onFloor[i] = ped != nil && !ped->bInVehicle &&
			(ped->GetPosition() - s_dance.centre).Magnitude2D() <= kDanceLeaveRange;
	}

	// The beat: whatever was not hit is a miss, then the next prompts appear.
	if(now >= s_dance.beatAt){
		for(int i = 0; i < NUMPLAYERS; i++)
			if(s_dance.prompt[i] >= 0){
				if(!s_dance.promptHit[i]){
					s_dance.combo[i] = 0;
					s_dance.missFlash[i] = now;
				}
				s_dance.prompt[i] = -1;
			}
		for(int i = 0; i < NUMPLAYERS; i++)
			if(onFloor[i]){
				s_dance.prompt[i] = CGeneral::GetRandomNumberInRange(0, DANCE_PROMPTS);
				// The nod is also the enter-vehicle button.  A dancer with a car
				// in reach is given jump or duck instead, or the prompt would
				// walk them off the floor and into it.
				if(s_dance.prompt[i] == DANCE_TWO && VehicleInReach(CCoop::GetPlayerPed(i)))
					s_dance.prompt[i] = CGeneral::GetRandomNumberInRange(0, (int)DANCE_TWO);
				s_dance.promptHit[i] = false;
			}
		s_dance.beatAt = now + DanceBeatInterval(now);
	}

	// The reads.  Jump and duck are the pad's own pulses; the nod is a
	// held state here, so its edge is tracked by hand.
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		CPad *pad = GetPadFromPlayer(ped);
		const bool triangle = pad->NewState.Triangle != 0;
		const bool triangleDown = triangle && !s_dance.triangleHeld[i];
		s_dance.triangleHeld[i] = triangle;

		if(!onFloor[i] || s_dance.prompt[i] < 0 || s_dance.promptHit[i])
			continue;
		bool hit = false;
		switch(s_dance.prompt[i]){
		case DANCE_JUMP: hit = pad->JumpJustDown(); break;
		case DANCE_DUCK: hit = pad->DuckJustDown(); break;
		default: hit = triangleDown; break;
		}
		if(hit){
			s_dance.promptHit[i] = true;
			s_dance.score[i]++;
			s_dance.combo[i]++;
			s_dance.hitFlash[i] = now;
		}
	}

	RegisterDanceCoronas();
}

// --- cops & robbers ----------------------------------------------------------

void
ClearCaseMarker(void)
{
	if(s_cnr.caseBlip >= 0){
		CRadar::ClearBlip(s_cnr.caseBlip);
		s_cnr.caseBlip = -1;
	}
	if(s_cnr.carrier >= 0){
		CPlayerPed *ped = CCoop::GetPlayerPed(s_cnr.carrier);
		if(ped != nil)
			CRadar::ClearBlipForEntity(BLIP_CHAR, CPools::GetPedPool()->GetIndex(ped));
	}
}

// The radar calls take a colour and ignore it (a coordinate blip is always
// magenta, a character's yellow), so each is set after, or the case and the
// getaway would look the same.
void
ShowLooseCase(void)
{
	s_cnr.caseBlip = CRadar::SetCoordBlip(BLIP_COORD, s_cnr.casePos, RADAR_TRACE_RED, BLIP_DISPLAY_BOTH);
	CRadar::ChangeBlipColour(s_cnr.caseBlip, RADAR_TRACE_RED);
}

void
ShowCarriedCase(void)
{
	CPlayerPed *ped = s_cnr.carrier >= 0 ? CCoop::GetPlayerPed(s_cnr.carrier) : nil;
	if(ped != nil){
		const int32 blip = CRadar::SetEntityBlip(BLIP_CHAR, CPools::GetPedPool()->GetIndex(ped), RADAR_TRACE_RED, BLIP_DISPLAY_BOTH);
		CRadar::ChangeBlipColour(blip, RADAR_TRACE_RED);
	}
}

void
DropCase(void)
{
	ClearCaseMarker();
	s_cnr.carried = false;
	s_cnr.carrier = -1;
	s_cnr.bustSince = 0;
	s_cnr.bustBy = -1;
	ShowLooseCase();
}

void
RegisterCnRCoronas(void)
{
	CCoronas::RegisterCorona((uintptr)&s_cnr, 255, 130, 70, 255,
		s_cnr.casePos, 3.0f, 200.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
		CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
		CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
	CCoronas::RegisterCorona((uintptr)&s_cnr + 1, 120, 255, 130, 255,
		s_cnr.getaway, 4.5f, 250.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
		CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
		CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
}

void
EndCnR(uint32 now, const char *why)
{
	ClearCaseMarker();
	if(s_cnr.getawayBlip >= 0){
		CRadar::ClearBlip(s_cnr.getawayBlip);
		s_cnr.getawayBlip = -1;
	}
	s_cnr.active = false;
	s_cnr.finished = false;
	s_cnr.rearmAt = now + kCnRRearmMs;
	COOP_LOG("WII coop: cops & robbers off (%s)\n", why);
}

void
FinishCnR(uint32 now, int winner)
{
	if(winner >= 0){
		s_cnr.winner = winner;
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += kCnRPot;
		COOP_LOG("WII coop: cops & robbers to player %d\n", winner + 1);
	}
	s_cnr.finished = true;
	s_cnr.finishAt = now + kCnRLingerMs;
}

void
StartCnR(uint32 now, const CVector &centre, const CVector &forward)
{
	s_cnr.active = true;
	s_cnr.finished = false;
	s_cnr.startAt = now;
	s_cnr.centre = centre;
	s_cnr.casePos = centre;
	s_cnr.carried = false;
	s_cnr.carrier = -1;
	s_cnr.bustSince = 0;
	s_cnr.bustBy = -1;
	s_cnr.lockPlayer = -1;
	s_cnr.lockUntil = 0;
	s_cnr.noteAt = 0;
	s_cnr.outSince = 0;
	s_cnr.winner = -1;
	s_cnr.caseBlip = -1;
	for(int i = 0; i < NUMPLAYERS; i++)
		s_cnr.score[i] = 0;

	CVector dir = forward;
	dir.z = 0.0f;
	if(dir.Magnitude() < 0.1f)
		dir = CVector(1.0f, 0.0f, 0.0f);
	dir.Normalise();
	s_cnr.getaway = centre + dir*kCnRGetawayDist;
	bool found = false;
	const float ground = CWorld::FindGroundZFor3DCoord(s_cnr.getaway.x, s_cnr.getaway.y, s_cnr.getaway.z + 2.0f, &found);
	s_cnr.getaway.z = (found ? ground : centre.z) + 1.0f;

	s_cnr.getawayBlip = CRadar::SetCoordBlip(BLIP_COORD, s_cnr.getaway, RADAR_TRACE_GREEN, BLIP_DISPLAY_BOTH);
	CRadar::ChangeBlipColour(s_cnr.getawayBlip, RADAR_TRACE_GREEN);
	ShowLooseCase();
	COOP_LOG("WII coop: cops & robbers on\n");
	CCoop::Notify("WII_CSN");
	ShowExplainer(EXPLAIN_CNR, now);
}

void
TryStartCnR(uint32 now, bool pressed)
{
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	if(now < s_cnr.rearmAt)
		return;
	if(!pressed)
		return;

	// The party on foot in a knot; the nod (Triangle) starts it.
	CVector sum(0.0f, 0.0f, 0.0f);
	CVector forward(0.0f, 0.0f, 0.0f);
	int count = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(ped->bInVehicle)
			return;
		sum += ped->GetPosition();
		forward += ped->GetForward();
		count++;
	}
	if(count < 2)
		return;
	const CVector centre = sum / (float)count;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil && (ped->GetPosition() - centre).Magnitude2D() > kCnRGatherRange)
			return;
	}
	StartCnR(now, centre, forward);
}

void
UpdateCnR(uint32 now)
{
	// The nod's holding edge, tracked every frame so a press during a
	// match is not read as one right after it.  The nod is also the
	// enter-vehicle button on foot, so a press from a player with a vehicle in
	// reach is somebody getting in and starts nothing; otherwise a party
	// walking up to a car would be starting a match.
	bool pressed = false;
	bool entering = false;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		CPad *pad = GetPadFromPlayer(ped);
		const bool tri = pad->NewState.Triangle != 0;
		if(tri && !s_cnr.triangleHeld[i]){
			if(s_cnr.active || !VehicleInReach(ped))
				pressed = true;
			else
				entering = true;
		}
		s_cnr.triangleHeld[i] = tri;
	}
	if(entering)
		pressed = false;

	if(!s_cnr.active){
		TryStartCnR(now, pressed);
		return;
	}
	if(!CCoop::PairActive()){
		EndCnR(now, "session");
		return;
	}
	if(s_cnr.finished){
		if(now >= s_cnr.finishAt)
			EndCnR(now, "over");
		return;
	}
	if(now - s_cnr.startAt >= kCnRCapMs){
		int best = -1;
		int bestScore = 0;
		bool tie = false;
		for(int i = 0; i < NUMPLAYERS; i++){
			if(s_cnr.score[i] > bestScore){
				best = i;
				bestScore = s_cnr.score[i];
				tie = false;
			}else if(s_cnr.score[i] == bestScore && s_cnr.score[i] > 0)
				tie = true;
		}
		FinishCnR(now, tie ? -1 : best);
		return;
	}

	// The case goes where its carrier goes; a carrier who is gone or down
	// drops it where it was.
	if(s_cnr.carried){
		CPlayerPed *carrier = s_cnr.carrier >= 0 ? CCoop::GetPlayerPed(s_cnr.carrier) : nil;
		if(carrier == nil || carrier->DyingOrDead())
			DropCase();
		else
			s_cnr.casePos = PlayerCoors(s_cnr.carrier);
	}

	// Loose: the closest player in reach takes it.
	if(!s_cnr.carried){
		int best = -1;
		float bestDist = 0.0f;
		for(int i = 0; i < NUMPLAYERS; i++){
			CPlayerPed *ped = CCoop::GetPlayerPed(i);
			if(ped == nil || ped->DyingOrDead())
				continue;
			// Just busted: not straight off their own feet, where they are the
			// nearest player by definition and would take it back on the next
			// frame -- and the buster score again 1.5 s after that.
			if(i == s_cnr.lockPlayer && now < s_cnr.lockUntil)
				continue;
			const float reach = (ped->bInVehicle && ped->m_pMyVehicle != nil) ? kCnRGrabRangeCar : kCnRGrabRange;
			const float dist = (PlayerCoors(i) - s_cnr.casePos).Magnitude2D();
			if(dist < reach && (best < 0 || dist < bestDist)){
				best = i;
				bestDist = dist;
			}
		}
		if(best >= 0){
			ClearCaseMarker();
			s_cnr.carrier = best;
			s_cnr.carried = true;
			s_cnr.casePos = PlayerCoors(best);
			ShowCarriedCase();
			s_cnr.bustSince = 0;
			s_cnr.bustBy = -1;
			COOP_LOG("WII coop: case to player %d\n", best + 1);
		}
	}

	if(s_cnr.carried){
		// Delivered to the getaway.
		if((s_cnr.casePos - s_cnr.getaway).Magnitude2D() < kCnRDeliverRange){
			const int scorer = s_cnr.carrier;
			s_cnr.score[scorer]++;
			COOP_LOG("WII coop: case delivered by player %d\n", scorer + 1);
			ClearCaseMarker();
			s_cnr.carried = false;
			s_cnr.carrier = -1;
			s_cnr.casePos = s_cnr.centre;
			ShowLooseCase();
			s_cnr.bustSince = 0;
			s_cnr.bustBy = -1;
			if(s_cnr.score[scorer] >= kCnRWinScore)
				FinishCnR(now, scorer);
			return;
		}

		// Busted: a cop holds them for a moment.
		int pin = -1;
		float pinDist = kCnRBustRange;
		for(int i = 0; i < NUMPLAYERS; i++){
			if(i == s_cnr.carrier)
				continue;
			CPlayerPed *ped = CCoop::GetPlayerPed(i);
			if(ped == nil || ped->DyingOrDead())
				continue;
			const float dist = (PlayerCoors(i) - s_cnr.casePos).Magnitude2D();
			if(dist < pinDist){
				pinDist = dist;
				pin = i;
			}
		}
		if(pin >= 0){
			if(pin != s_cnr.bustBy){
				s_cnr.bustBy = pin;
				s_cnr.bustSince = now;
			}else if(now - s_cnr.bustSince >= kCnRBustHoldMs){
				const int busted = s_cnr.carrier;
				s_cnr.score[pin]++;
				COOP_LOG("WII coop: player %d busted player %d\n", pin + 1, busted + 1);
				DropCase();
				s_cnr.lockPlayer = busted;
				s_cnr.lockUntil = now + kCnRBustLockMs;
				s_cnr.noteBuster = pin;
				s_cnr.noteBusted = busted;
				s_cnr.noteAt = now;
				if(s_cnr.score[pin] >= kCnRWinScore)
					FinishCnR(now, pin);
				return;
			}
		}else{
			s_cnr.bustSince = 0;
			s_cnr.bustBy = -1;
		}

		// Out of the field: the case comes back, no score either way.
		if((s_cnr.casePos - s_cnr.centre).Magnitude2D() > kCnRArena){
			if(s_cnr.outSince == 0)
				s_cnr.outSince = now;
			else if(now - s_cnr.outSince >= kCnROutGraceMs){
				ClearCaseMarker();
				s_cnr.carried = false;
				s_cnr.carrier = -1;
				s_cnr.casePos = s_cnr.centre;
				s_cnr.outSince = 0;
				ShowLooseCase();
			}
		}else{
			s_cnr.outSince = 0;
		}
	}

	RegisterCnRCoronas();
}

// --- the screen marks --------------------------------------------------------

// Where a player's mark sits along the bottom lane: the cops & robbers, derby
// and race bars and the dance chips all keep the same slots, so a player is
// in the same place whichever mode it is.
//
// The numbers, at 640x448 (4:3 or 16:9 -- the framebuffer is the same and a
// wide TV stretches it, with SCREEN_SCALE_X narrowing everything drawn by 3/4
// to match): the lanes are centred on x = 176, 272, 368 and 464.  The radar
// disc ends at x = 128 (96 at 16:9) and a bar starts at 144 (151 at 16:9), so
// they never touch, and the widest dance chip, the remote's D-PAD DOWN,
// starts at 129 (140): clear by a pixel at 4:3.  Vertically the lane is
// y = 365..388 with the tag above it and the caption below, 345..404, and
// the game's own subtitle starts at y = 348 and takes about 21 px a line --
// so while one is up the whole lane steps above it, to y = 285..345.  The
// zone and vehicle names, which would otherwise be there, are held back by
// the HUD for as long as a subtitle is.
void
LaneSlot(int player, float &cx, float &cy)
{
	cx = SCREEN_WIDTH*(0.5f + (player - 1.5f)*0.15f);
	cy = SCREEN_HEIGHT*0.84f;
	if(CHud::m_Message[0] != 0)
		cy -= SCREEN_SCALE_Y(64.0f);
}

// A few characters, centred on x, in the colour given.  The text is plain
// ASCII on purpose -- a number, P2, GO -- and short; the same as the chips.
// AsciiToUnicode does not look at the size of what it writes into, so what
// goes in here is short enough for the buffer.
void
PrintCentred(float x, float y, const char *ascii, const CRGBA &colour, float scaleX, float scaleY)
{
	wchar text[48];
	AsciiToUnicode(ascii, text);
	CFont::SetPropOn();
	CFont::SetBackgroundOff();
	CFont::SetScale(SCREEN_SCALE_X(scaleX), SCREEN_SCALE_Y(scaleY));
	CFont::SetJustifyOff();
	CFont::SetCentreOn();
	CFont::SetCentreSize(SCREEN_SCALE_X(560.0f));
	CFont::SetFontStyle(FONT_STANDARD);
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));
	CFont::SetColor(colour);
	CFont::PrintString(x, y, text);
	CFont::SetCentreOff();
}

// The label colour that reads on a fill: black on a light one, white on a dark
// one.  The player colours are all pastels, and white on them does not read.
CRGBA
InkFor(const CRGBA &fill)
{
	const int luma = (fill.r*299 + fill.g*587 + fill.b*114)/1000;
	return luma >= 128 ? CRGBA(0, 0, 0, 255) : CRGBA(255, 255, 255, 255);
}

// A player's name over their lane, in their colour.  The colour alone is a
// code nobody has been given the key to.
void
DrawLaneTag(int player, const char *text)
{
	float cx, cy;
	LaneSlot(player, cx, cy);
	PrintCentred(cx, cy - SCREEN_SCALE_Y(33.0f), text, kCoopColours[player], 0.55f, 0.85f);
}

// A player's bar in the lane, in their colour, filled to a fraction.  It is
// cut into one cell for each thing being counted -- a round to win, a point,
// a lap -- with a gap between them, so that a bar that is half full reads as
// one of two and not as half of something; and over it, the player's name.
void
DrawLaneBar(int player, float fill, int cells)
{
	float cx, cy;
	LaneSlot(player, cx, cy);
	const float hw = SCREEN_SCALE_X(30.0f);
	const float hh = SCREEN_SCALE_Y(10.0f);
	const float gap = 2.0f;
	CSprite2d::DrawRect(CRect(cx - hw - 2.0f, cy - hh - 2.0f, cx + hw + 2.0f, cy + hh + 2.0f), CRGBA(0, 0, 0, 255));
	fill = Clamp(fill, 0.0f, 1.0f)*(float)cells;
	for(int cell = 0; cell < cells; cell++){
		const float left = cx - hw + 2.0f*hw*(float)cell/(float)cells + (cell > 0 ? gap*0.5f : 0.0f);
		const float right = cx - hw + 2.0f*hw*(float)(cell + 1)/(float)cells - (cell < cells - 1 ? gap*0.5f : 0.0f);
		CSprite2d::DrawRect(CRect(left, cy - hh, right, cy + hh), CRGBA(70, 70, 70, 255));
		const float part = Clamp(fill - (float)cell, 0.0f, 1.0f);
		if(part > 0.0f)
			CSprite2d::DrawRect(CRect(left, cy - hh, left + (right - left)*part, cy + hh), kCoopColours[player]);
	}
	char name[8];
	sprintf(name, "P%d", player + 1);
	DrawLaneTag(player, name);
}

// The mode's headline, near the top and out of the way of the help box and
// the money.
void
DrawStatus(const char *line, const CRGBA &colour)
{
	PrintCentred(SCREEN_WIDTH*0.5f, SCREEN_SCALE_Y(36.0f), line, colour, 0.7f, 1.0f);
}

// The seconds left to a bell, large, under the headline.  Nothing once it has
// rung.
void
DrawCountdown(uint32 now, uint32 bellAt)
{
	if(now >= bellAt)
		return;
	char digits[16];
	sprintf(digits, "%u", (unsigned)((bellAt - now + 999)/1000));
	PrintCentred(SCREEN_WIDTH*0.5f, SCREEN_SCALE_Y(70.0f), digits, CRGBA(255, 255, 255, 255), 1.6f, 2.2f);
}

// "GO" for the first second after the bell.
void
DrawGo(uint32 now, uint32 bellAt)
{
	if(now >= bellAt && now - bellAt < 1000)
		PrintCentred(SCREEN_WIDTH*0.5f, SCREEN_SCALE_Y(70.0f), "GO", CRGBA(120, 255, 130, 255), 1.6f, 2.2f);
}

// Who took it, in their colour, for as long as the mode lingers after the end.
void
DrawWinner(int winner)
{
	if(winner < 0 || winner >= NUMPLAYERS){
		DrawStatus("NO WINNER", CRGBA(255, 255, 255, 255));
		return;
	}
	char line[16];
	sprintf(line, "P%d WINS", winner + 1);
	DrawStatus(line, kCoopColours[winner]);
}

// The first-arming explainer: one line under the headline and the countdown,
// on a dim band so it reads over sky and sea alike, for as long as the mode it
// is for is still on.  Each is written from the rules in this file -- what
// scores, what drops a player out, what ends it -- and has to be short enough
// to read in kExplainMs.  It scales itself down to fit the screen if it must.
void
DrawExplainer(uint32 now)
{
	static const char *const kLines[EXPLAIN_COUNT] = {
		"CHASE THE RADAR BLIP. STOP IT OR WRECK IT. SIREN OFF TO QUIT",
		"STOP AT EACH DROP MARKER TO UNLOAD. 3 DROPS. LEAVE THE BOAT TO QUIT",
		"RAMMING ONLY, NO SHOOTING. LAST CAR IN THE RING WINS THE ROUND. FIRST TO 2",
		"GET IN A TOY AND FOLLOW YOUR COLOR OF GATE. FIRST TO 3 LAPS WINS",
		"PRESS THE BUTTON ON YOUR CHIP BEFORE THE NEXT BEAT. BEST SCORE WINS",
		"GRAB THE CASE, RUN IT TO THE GREEN MARKER. COPS STAY CLOSE TO BUST. FIRST TO 3"
	};

	if(s_explain.until == 0 || now >= s_explain.until)
		return;
	bool live;
	switch(s_explain.showing){
	case EXPLAIN_SHIFT: live = s_shift.active; break;
	case EXPLAIN_RUN: live = s_run.active; break;
	case EXPLAIN_DERBY: live = s_derby.active; break;
	case EXPLAIN_RC: live = s_rc.active; break;
	case EXPLAIN_DANCE: live = s_dance.active; break;
	default: live = s_cnr.active; break;
	}
	if(!live)
		return;

	wchar text[96];
	AsciiToUnicode(kLines[s_explain.showing], text);
	CFont::SetPropOn();
	CFont::SetBackgroundOff();
	CFont::SetJustifyOff();
	CFont::SetFontStyle(FONT_STANDARD);
	float scaleX = SCREEN_SCALE_X(0.4f);
	const float scaleY = SCREEN_SCALE_Y(0.82f);
	CFont::SetScale(scaleX, scaleY);
	float width = CFont::GetStringWidth(text, true);
	const float room = SCREEN_WIDTH - SCREEN_SCALE_X(40.0f);
	if(width > room){
		scaleX *= room/width;
		width = room;
		CFont::SetScale(scaleX, scaleY);
	}

	const float x = SCREEN_WIDTH*0.5f;
	// Below the countdown's digits (y = 65..106) and the wanted stars at the
	// top right (to y = 107).
	const float y = SCREEN_SCALE_Y(124.0f);
	const float pad = SCREEN_SCALE_X(8.0f);
	CSprite2d::DrawRect(CRect(x - width*0.5f - pad, y - 3.0f, x + width*0.5f + pad, y + 20.0f*scaleY + 3.0f), CRGBA(0, 0, 0, 150));
	CFont::SetCentreOn();
	CFont::SetCentreSize(room);
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));
	CFont::SetColor(CRGBA(255, 255, 255, 255));
	CFont::PrintString(x, y, text);
	CFont::SetCentreOff();
}

// The cop shift's marks: the streak and the seconds the open case has left,
// and for a moment after a case closes or the shift stands down, what it came
// to.  The result sits in the countdown's place, under the headline.
void
DrawShiftMarks(uint32 now)
{
	if(s_shift.active){
		char line[32];
		if(s_shift.streak > 0)
			sprintf(line, "COP SHIFT x%u", (unsigned)s_shift.streak);
		else
			sprintf(line, "COP SHIFT");
		bool late = false;
		if(s_shift.caseActive){
			const uint32 age = now - s_shift.caseStartAt;
			const uint32 left = age < kCaseTimeoutMs ? (kCaseTimeoutMs - age + 999)/1000 : 0;
			sprintf(line + strlen(line), "  %us", (unsigned)left);
			late = left <= 15;
		}
		DrawStatus(line, late ? CRGBA(255, 200, 90, 255) : CRGBA(255, 255, 255, 255));
	}

	if(s_shift.resultAt == 0 || now - s_shift.resultAt >= kShiftResultMs)
		return;
	char result[24];
	CRGBA colour(255, 255, 255, 255);
	switch(s_shift.resultKind){
	case SHIFT_RESULT_CAUGHT:
		sprintf(result, "CAUGHT  $%u", (unsigned)s_shift.resultPay);
		colour = CRGBA(120, 255, 130, 255);
		break;
	case SHIFT_RESULT_AWAY:
		sprintf(result, "GOT AWAY");
		colour = CRGBA(255, 200, 90, 255);
		break;
	case SHIFT_RESULT_SIREN:
		sprintf(result, "SIREN OFF");
		break;
	case SHIFT_RESULT_STOOD_DOWN:
		sprintf(result, "OFF DUTY");
		break;
	default:
		return;
	}
	PrintCentred(SCREEN_WIDTH*0.5f, SCREEN_SCALE_Y(70.0f), result, colour, 1.0f, 1.5f);
}

// Everything a mode shows on screen that is not a chip: the headline, the
// countdown and the bars.  A mode that has ended keeps its marks for the
// moment it lingers, so the result is on screen and not just in the log.
void
DrawModeMarks(uint32 now)
{
	char line[32];
	const CRGBA white(255, 255, 255, 255);

	// The cop shift: the streak, the case's clock, and what the last case came to.
	DrawShiftMarks(now);

	// The smuggling run: which drop, and when the boat is unloading.
	if(s_run.active){
		if(s_run.unloadSince != 0)
			sprintf(line, "UNLOADING %d/%d", s_run.leg + 1, kDrops);
		else
			sprintf(line, "DROP %d/%d", s_run.leg + 1, kDrops);
		DrawStatus(line, s_run.unloadSince != 0 ? CRGBA(255, 200, 90, 255) : white);
	}

	// The derby: the round and the bell, then how many cars are still running;
	// a bar per player, a cell for each round they have won.
	if(s_derby.active){
		if(s_derby.finished){
			DrawWinner(s_derby.matchWinner);
		}else if(s_derby.counting){
			// The fire button is off for everyone for the whole match.  The
			// explainer says so once; this says so every time.
			sprintf(line, "ROUND %d  RAMMING ONLY", s_derby.round + 1);
			DrawStatus(line, white);
			DrawCountdown(now, s_derby.countdownAt);
		}else{
			int left = 0;
			for(int i = 0; i < NUMPLAYERS; i++)
				if(s_derby.running[i])
					left++;
			sprintf(line, "ROUND %d  %d LEFT", s_derby.round, left);
			DrawStatus(line, white);
			DrawGo(now, s_derby.roundStartAt);
		}
		for(int i = 0; i < NUMPLAYERS; i++)
			if(CCoop::GetPlayerPed(i) != nil)
				DrawLaneBar(i, (float)s_derby.roundWins[i]/(float)kDerbyWinsNeeded, kDerbyWinsNeeded);
	}

	// The race: the lights, then a bar per racer for how far round they are,
	// a cell to a lap, and which lap they are on.
	if(s_rc.active){
		if(s_rc.finished)
			DrawWinner(s_rc.winner);
		else if(s_rc.grid)
			DrawStatus("GET IN A TOY", white);
		else if(s_rc.counting){
			DrawStatus("RC RACE", white);
			DrawCountdown(now, s_rc.countdownAt);
		}else
			DrawGo(now, s_rc.countdownAt);
		for(int i = 0; i < NUMPLAYERS; i++){
			if(s_rc.nextGate[i] < 0)
				continue;
			DrawLaneBar(i, (float)(s_rc.lap[i]*kRcGates + s_rc.nextGate[i])/(float)(kRcLaps*kRcGates), kRcLaps);
			float cx, cy;
			LaneSlot(i, cx, cy);
			sprintf(line, "LAP %d/%d", Min(s_rc.lap[i] + 1, kRcLaps), kRcLaps);
			PrintCentred(cx, cy + SCREEN_SCALE_Y(16.0f), line, white, 0.45f, 0.7f);
		}
	}

	// The dance-off: the time left, then each dancer's name and hits above their
	// slot; the chips themselves are drawn by DrawHud.
	if(s_dance.active){
		if(s_dance.finished){
			DrawWinner(s_dance.winner);
		}else{
			const uint32 elapsed = now - s_dance.startAt;
			sprintf(line, "DANCE-OFF %u", (unsigned)(elapsed < kDanceRoundMs ? (kDanceRoundMs - elapsed + 999)/1000 : 0));
			DrawStatus(line, white);
		}
		for(int i = 0; i < NUMPLAYERS; i++){
			if(CCoop::GetPlayerPed(i) == nil)
				continue;
			sprintf(line, "P%d  %d", i + 1, s_dance.score[i]);
			DrawLaneTag(i, line);
		}
	}

	// Cops & robbers: who has the case, and each score as a bar of three cells,
	// three to win.  A bust is the headline for a moment: who busted whom.
	if(s_cnr.active){
		if(s_cnr.finished){
			DrawWinner(s_cnr.winner);
		}else if(s_cnr.noteAt != 0 && now - s_cnr.noteAt < kCnRNoteMs){
			sprintf(line, "P%d BUSTED P%d", s_cnr.noteBuster + 1, s_cnr.noteBusted + 1);
			DrawStatus(line, kCoopColours[s_cnr.noteBuster]);
		}else if(s_cnr.carried && s_cnr.carrier >= 0){
			sprintf(line, "P%d HAS THE CASE", s_cnr.carrier + 1);
			DrawStatus(line, kCoopColours[s_cnr.carrier]);
		}else{
			DrawStatus("GRAB THE CASE", white);
		}
		for(int i = 0; i < NUMPLAYERS; i++)
			if(CCoop::GetPlayerPed(i) != nil)
				DrawLaneBar(i, (float)s_cnr.score[i]/(float)kCnRWinScore, kCnRWinScore);
	}

	// And, the first time the mode armed this session, how it is played.
	DrawExplainer(now);
}

} // namespace

void
CCoopModes::Init(void)
{
	if(s_shift.suspectCar != nil)
		s_shift.suspectCar->CleanUpOldReference((CEntity**)&s_shift.suspectCar);
	if(s_shift.suspectPed != nil)
		s_shift.suspectPed->CleanUpOldReference((CEntity**)&s_shift.suspectPed);
	memset(&s_shift, 0, sizeof(s_shift));

	if(s_run.boat != nil)
		s_run.boat->CleanUpOldReference((CEntity**)&s_run.boat);
	for(int i = 0; i < 2; i++){
		if(s_run.pursuer[i] != nil)
			s_run.pursuer[i]->CleanUpOldReference((CEntity**)&s_run.pursuer[i]);
		if(s_run.pursuerDriver[i] != nil)
			s_run.pursuerDriver[i]->CleanUpOldReference((CEntity**)&s_run.pursuerDriver[i]);
	}
	memset(&s_run, 0, sizeof(s_run));
	// A cleared blip has to read as "none": id 0 is a real blip.
	s_run.blip = -1;

	for(int i = 0; i < NUMPLAYERS; i++){
		if(s_derby.loaner[i] != nil)
			s_derby.loaner[i]->CleanUpOldReference((CEntity**)&s_derby.loaner[i]);
	}
	memset(&s_derby, 0, sizeof(s_derby));
	s_derby.matchWinner = -1;

	for(int i = 0; i < NUMPLAYERS; i++){
		if(s_rc.car[i] != nil)
			s_rc.car[i]->CleanUpOldReference((CEntity**)&s_rc.car[i]);
	}
	memset(&s_rc, 0, sizeof(s_rc));
	s_rc.winner = -1;

	memset(&s_dance, 0, sizeof(s_dance));
	s_dance.winner = -1;
	for(int i = 0; i < NUMPLAYERS; i++)
		s_dance.prompt[i] = -1;

	memset(&s_cnr, 0, sizeof(s_cnr));
	s_cnr.carrier = -1;
	s_cnr.winner = -1;
	s_cnr.caseBlip = -1;
	s_cnr.getawayBlip = -1;
	s_cnr.lockPlayer = -1;
	s_cnr.noteBuster = -1;
	s_cnr.noteBusted = -1;

	memset(&s_explain, 0, sizeof(s_explain));
}

void
CCoopModes::Update(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();

	// The explainers are for the first time a mode arms in a session.  The
	// session is what every mode below asks CCoop::PairActive about, and it is
	// over for a mission, a cutscene, player 1 down, or the party gone: the
	// next arming is a new session's, and is told again.
	if(!CCoop::PairActive())
		memset(&s_explain, 0, sizeof(s_explain));

	// The modes are independent; all ride the same session state.
	UpdateRun(now);
	UpdateDerby(now);
	UpdateRc(now);
	UpdateDance(now);
	UpdateCnR(now);

	// A resolved case lets go of its suspect on its own clock, shift or no
	// shift: the wreck and the body linger a moment before the world takes
	// over.
	if(s_shift.cleanupPending && now >= s_shift.cleanupAt)
		RunCleanup();

	if(!s_shift.active){
		TryStartShift(now);
		return;
	}

	if(!CCoop::PairActive()){
		EndShift(now, "session", SHIFT_RESULT_NONE);
		return;
	}

	// The siren is the switch.  The crew sitting in a law car with it off --
	// case or no case -- is the party standing down.
	if(FindCrewedLawCar(false) != nil && FindCrewedLawCar(true) == nil){
		EndShift(now, "siren off", SHIFT_RESULT_SIREN);
		return;
	}

	if(s_shift.caseActive){
		UpdateCase(now);
		return;
	}

	// The switch gates what starts, and a case is what starts here: with
	// MINIGAMES off the open case was played out above and the shift is over,
	// rather than raising suspects for as long as the siren stays on.
	if(!CoopMinigames){
		EndShift(now, "switched off", SHIFT_RESULT_STOOD_DOWN);
		return;
	}

	// No case open: the shift lives on a crewed cruiser with the siren on.
	// Scattered or on foot, it has a moment to get back in.
	CVehicle *cruiser = FindCrewedLawCar(true);
	if(cruiser == nil){
		if(s_shift.crewLostAt == 0)
			s_shift.crewLostAt = now;
		else if(now - s_shift.crewLostAt >= 5000)
			EndShift(now, "no crew", SHIFT_RESULT_STOOD_DOWN);
		return;
	}
	s_shift.crewLostAt = 0;

	if(!s_shift.spawnPending && !s_shift.cleanupPending && now >= s_shift.nextCaseAt)
		StartCase(now);
}

bool
CCoopModes::OnDuty(CEntity *criminal, CEntity *victim, bool gunfire)
{
	if(s_shift.suspectCar == nil && s_shift.suspectPed == nil)
		return false;
	if(victim != nil && (victim == s_shift.suspectCar || victim == s_shift.suspectPed))
		return true;
	if(gunfire && criminal != nil && s_shift.suspectCar != nil &&
	   (criminal->GetPosition() - s_shift.suspectCar->GetPosition()).Magnitude() < kGunfireRange)
		return true;
	return false;
}

bool
CCoopModes::SteadyView(void)
{
	// The derby, where every car is spinning, and the toy race, where every
	// car is a twitchy little thing that would whip the view around.  See
	// CCam::Process_WiiCoop.
	return s_derby.active || s_rc.active;
}

bool
CCoopModes::FreeRoam(void)
{
	// Cops & robbers is a pursuit: the wall that holds the party together has
	// to stand aside or nobody can outrun anybody.  See CCoop::LimitSeparation
	// and NeedsRegroup.
	return s_cnr.active;
}

bool
CCoopModes::IsModeVehicle(const CVehicle *vehicle)
{
	if(vehicle == nil)
		return false;
	if(vehicle == s_shift.suspectCar)
		return true;
	for(int i = 0; i < NUMPLAYERS; i++){
		if(vehicle == s_derby.loaner[i] || vehicle == s_rc.car[i])
			return true;
	}
	return vehicle == s_run.pursuer[0] || vehicle == s_run.pursuer[1];
}

void
CCoopModes::DrawHud(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();

	// The headlines, countdowns and bars, and what is left of them once a mode
	// has ended.
	DrawModeMarks(now);

	if(!s_dance.active || s_dance.finished)
		return;

	// What each prompt is called; the chip itself holds the button.
	static const char *const actionNames[DANCE_PROMPTS] = { "JUMP", "DUCK", "NOD" };

	for(int i = 0; i < NUMPLAYERS; i++){
		const bool missed = now - s_dance.missFlash[i] < 250;
		if(s_dance.prompt[i] < 0 && !missed)
			continue;
		float cx, cy;
		LaneSlot(i, cx, cy);
		const float hh = SCREEN_SCALE_Y(10.0f);

		// The player's own controller's button, when the label helper can name
		// it: jump is X, Y and 1 across the GameCube pad, the Classic and the
		// remote; duck R, ZL and D-pad down; the nod Y, X and 2.  A player the
		// helper cannot name keeps the action word.  When it can, the action
		// word goes under the chip, so a player is told what to do as well as
		// which button does it.
		wchar text[16];
		float hw = SCREEN_SCALE_X(30.0f);
		const char *named = nil;
		if(s_dance.prompt[i] >= 0){
			const char *label = actionNames[s_dance.prompt[i]];
#ifdef NINTENDO_WII
			const WiiPadPrompt button = s_dance.prompt[i] == DANCE_JUMP ? WII_PROMPT_JUMP :
			                            s_dance.prompt[i] == DANCE_DUCK ? WII_PROMPT_DUCK :
			                            WII_PROMPT_NOD;
			named = WiiPadPromptButton(i, button);
			if(named != nil)
				label = named;
#endif
			AsciiToUnicode(label, text);

			// The chip holds its label: the remote's duck, "D-PAD DOWN", draws
			// half again the base width, so it grows -- capped before two lanes
			// could ever touch.  Spaces count: the fixed-width font advances
			// them like any other character.
			CFont::SetPropOff();
			CFont::SetBackgroundOff();
			CFont::SetScale(SCREEN_SCALE_X(0.55f), SCREEN_SCALE_Y(1.0f));
			CFont::SetJustifyOff();
			CFont::SetCentreOn();
			CFont::SetCentreSize(9999.0f);
			CFont::SetFontStyle(FONT_STANDARD);
			hw = Clamp(CFont::GetStringWidth(text, true)*0.5f + SCREEN_SCALE_X(4.0f),
			           hw, SCREEN_SCALE_X(45.0f));
		}

		// A miss rings the slot red until the next prompt settles in.
		const CRGBA backdrop = missed ? CRGBA(170, 40, 40, 255) : CRGBA(0, 0, 0, 255);
		CSprite2d::DrawRect(CRect(cx - hw - 2.0f, cy - hh - 2.0f, cx + hw + 2.0f, cy + hh + 2.0f), backdrop);
		if(s_dance.prompt[i] < 0)
			continue;

		CRGBA fill = kCoopColours[i];
		if(now - s_dance.hitFlash[i] < 250)
			fill = CRGBA(255, 255, 255, 255);
		CSprite2d::DrawRect(CRect(cx - hw, cy - hh, cx + hw, cy + hh), fill);

		// Dark on the light fill, no shadow: the player colours are pastels, and
		// a white label on them is about 1.4 to 2.5 to 1.  The label sits with
		// the middle of its capitals on the middle of the chip.
		CFont::SetDropShadowPosition(0);
		CFont::SetColor(InkFor(fill));
		CFont::PrintString(cx, cy - SCREEN_SCALE_Y(10.0f), text);
		CFont::SetCentreOff();
		CFont::SetDropShadowPosition(1);

		// The action under the chip, in the player's colour, with the run of hits
		// they are on once it is more than a couple.
		if(named != nil){
			char caption[24];
			if(s_dance.combo[i] >= 3)
				sprintf(caption, "%s x%d", actionNames[s_dance.prompt[i]], s_dance.combo[i]);
			else
				sprintf(caption, "%s", actionNames[s_dance.prompt[i]]);
			PrintCentred(cx, cy + SCREEN_SCALE_Y(16.0f), caption, kCoopColours[i], 0.45f, 0.7f);
		}
	}
}
