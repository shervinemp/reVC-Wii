#include "common.h"

#include "CoopModes.h"

#include "Automobile.h"
#include "AutoPilot.h"
#include "CarCtrl.h"
#include "CivilianPed.h"
#include "Coop.h"
#include "General.h"
#include "Hud.h"
#include "maths.h"
#include "ModelIndices.h"
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

} // namespace

void
CCoopModes::Init(void)
{
	if(s_shift.suspectCar != nil)
		s_shift.suspectCar->CleanUpOldReference((CEntity**)&s_shift.suspectCar);
	if(s_shift.suspectPed != nil)
		s_shift.suspectPed->CleanUpOldReference((CEntity**)&s_shift.suspectPed);
	memset(&s_shift, 0, sizeof(s_shift));
}

void
CCoopModes::Update(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();

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
