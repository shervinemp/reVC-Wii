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
const float kStopSpeed = 0.6f;
const uint32 kStopHoldMs = 2000;
const uint32 kCleanupDelayMs = 4000;	// how long a resolved case lingers
const float kGunfireRange = 60.0f;	// on duty: gunfire this near the case
const int kCaseCars[] = { MI_SENTINEL, MI_STALLION, MI_VOODOO, MI_SABRE };

// --- the smuggling run's numbers ---------------------------------------------

const uint32 kRearmMs = 8000;		// after a run, before the horn arms again
const int kDrops = 3;
const float kLegDistance = 320.0f;	// water between drops
const float kDropSearch = 300.0f;	// how far a leg may look for water
const float kDropRadius = 35.0f;	// in the ring...
const float kDropSpeed = 4.0f;		// ...and this slow to unload
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
const float kDerbyRadius = 14.0f;	// the ring itself
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
// the mission-ped count, and the car takes the DELETE_CAR door.
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
		COOP_LOG("WII coop: case closed, streak %u\n", (unsigned)s_shift.streak);
	}else{
		s_shift.streak = 0;
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
EndShift(uint32 now, const char *why)
{
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
	// The law's view of the party: nobody starts a shift with a record.
	CPlayerPed *lead = FindPlayerPed();
	if(lead == nil || lead->m_pWanted == nil || lead->m_pWanted->GetWantedLevel() > 0)
		return;
	if(FindCrewedLawCar(true) == nil)
		return;

	s_shift.active = true;
	s_shift.streak = 0;
	s_shift.crewLostAt = 0;
	s_shift.nextCaseAt = now + kShiftStartDelayMs;
	COOP_LOG("WII coop: cop shift on\n");
	if(CHud::m_HelpMessage[0] == 0 && CHud::m_HelpMessageState == 0)
		CHud::SetHelpMessage(TheText.Get("WII_CSH"), true);
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
// heading, each on water: the run follows the coast wherever it starts.
bool
PlanRoute(CVehicle *boat)
{
	CVector prev = boat->GetPosition();
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
			if(FindWaterNodeNear(prev + ndir * kLegDistance, kDropSearch, s_run.dropPos[leg])){
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

// Chase the smuggling boat: the same drive-to-coords call the scripts use for
// any vehicle, on a water node beside the boat, re-aimed on a clock.  The
// path search is handed the vehicle, so a boat's route stays on water.
void
DrivePursuer(int index, uint32 now, CVehicle *boat)
{
	CVehicle *predator = s_run.pursuer[index];
	if(predator == nil || boat == nil)
		return;
	CVector dest;
	if(!FindWaterNodeNear(boat->GetPosition(), 150.0f, dest))
		dest = boat->GetPosition();
	if(CCarCtrl::JoinCarWithRoadSystemGotoCoors(predator, dest, false))
		predator->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS_STRAIGHT;
	else
		predator->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS;
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
	if(CHud::m_HelpMessage[0] == 0 && CHud::m_HelpMessageState == 0)
		CHud::SetHelpMessage(TheText.Get("WII_CSR"), true);
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

// A Bloodring Banger beside a player who needs one, parked, locked, ready.
// Handed back to the world when the derby is over, like the suspect and the
// Predators.
void
SpawnLoaner(int player)
{
	if(s_derby.loaner[player] != nil)
		return;
	if(!CStreaming::HasModelLoaded(MI_BLOODRA))
		return;
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	if(ped == nil)
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
	CTheScripts::ClearSpaceForMissionEntity(pos, car);
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
	if(CHud::m_HelpMessage[0] == 0 && CHud::m_HelpMessageState == 0)
		CHud::SetHelpMessage(TheText.Get("WII_CSD"), true);
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
	for(int i = 0; i < NUMPLAYERS; i++){
		CVehicle *car = s_derby.loaner[i];
		if(car != nil){
			CTheScripts::CleanUpThisVehicle(car);
			car->CleanUpOldReference((CEntity**)&s_derby.loaner[i]);
			s_derby.loaner[i] = nil;
		}
	}
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
		if(car[i]->GetMoveSpeed().Magnitude() > 0.6f ||
		   (car[i]->GetPosition() - centre).Magnitude2D() > kDerbyGatherRange)
			return;
	}

	// A honk from any of them is the nod.
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
		if(veh == nil || veh->GetStatus() == STATUS_WRECKED)
			SpawnLoaner(i);
	}
	s_derby.counting = true;
	s_derby.countdownAt = now + kDerbyIntermissionMs;
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
}

void
CCoopModes::Update(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();

	// The modes are independent; all ride the same session state.
	UpdateRun(now);
	UpdateDerby(now);

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
		EndShift(now, "session");
		return;
	}

	// The siren is the switch.  The crew sitting in a law car with it off --
	// case or no case -- is the party standing down.
	if(FindCrewedLawCar(false) != nil && FindCrewedLawCar(true) == nil){
		EndShift(now, "siren off");
		return;
	}

	if(s_shift.caseActive){
		UpdateCase(now);
		return;
	}

	// No case open: the shift lives on a crewed cruiser with the siren on.
	// Scattered or on foot, it has a moment to get back in.
	CVehicle *cruiser = FindCrewedLawCar(true);
	if(cruiser == nil){
		if(s_shift.crewLostAt == 0)
			s_shift.crewLostAt = now;
		else if(now - s_shift.crewLostAt >= 5000)
			EndShift(now, "no crew");
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
	// The derby: a ring of spinning cars, and a camera that turns with one of
	// them turns with all of them.  See CCam::Process_WiiCoop.
	return s_derby.active;
}
