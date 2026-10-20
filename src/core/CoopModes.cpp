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
#include "Frontend.h"
#include "GameLogic.h"
#include "General.h"
#include "Hud.h"
#include "maths.h"
#include "ModelIndices.h"
#include "Pad.h"
#include "PathFind.h"
#include "Ped.h"
#include "PedPlacement.h"
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
#include "WaterLevel.h"
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

enum
{
	MENU_SHIFT = CCoopModes::MENU_SHIFT,
	MENU_RUN = CCoopModes::MENU_RUN,
	MENU_DERBY = CCoopModes::MENU_DERBY,
	MENU_RC = CCoopModes::MENU_RC,
	MENU_DANCE = CCoopModes::MENU_DANCE,
	MENU_CNR = CCoopModes::MENU_CNR,
	MENU_PITCH = CCoopModes::MENU_PITCH,
	MENU_KOTH = CCoopModes::MENU_KOTH
};

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
const float kCargoDistance = 90.0f;	// the crate waits this far before the drop...
const float kCargoSearch = 120.0f;	// ...on water near there
const float kCargoRadius = 10.0f;	// either boat this close picks it up
// Three drops that planned out within this much of the boat, or of each other,
// would pay out where the boat already is: a drop that near is not a drop.
const float kMinDropSeparation = 150.0f;
const uint32 kUnloadMs = 2000;
const float kPursuerDistance = 130.0f;	// heat spawns this far out
const uint32 kPursuerRetargetMs = 3000;
const uint32 kCrewGraceMs = 5000;	// the skipper may be out of the boat this long
const float kEscortRange = 80.0f;	// a partner's boat this near the lead's is the crew
const float kEscortGap = 14.0f;		// an escort appears this far to the side of the lead boat...
const float kEscortBack = 8.0f;		// ...and this far astern, a boat length more for each partner
const int kDropPay = 100;
const int kEscortPay = 50;		// on top of it, for each boat of the crew at a drop

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

// The run: player 1's boat, the three drops planned from where it armed, the
// boats the rest of the party is given to drive alongside it, and the two
// police boats the heat brings.  Same rule as the shift: every entity is a
// registered reference, and nil means gone.  Slot 0 of the escorts is player
// 1's and stays empty.
struct RunState
{
	bool active;
	uint32 rearmAt;
	int leg;
	CVector dropPos[kDrops];
	CVector cargoPos[kDrops];	// where this leg's cargo is waiting
	CVector drop;
	int32 blip;
	int32 cargoBlip;		// cargo waiting: the yellow coordinate blip
	int32 objectiveBlip;	// cargo picked: the carrier's boat, in green
	int32 objectiveHandle;
	bool cargoPlanned;		// this leg's crate spot has been worked out
	bool firstHeat;			// the first Predator has been sent out
	int carrier;			// player whose boat is carrying the cargo, or -1
	int32 unloadBoat;	// pool index of the boat the timer belongs to
	uint32 unloadSince;
	uint32 crewLostAt;
	uint32 nextRetargetAt;
	CVehicle *boat;
	CVehicle *escort[NUMPLAYERS];
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

// --- Caddy football's numbers ------------------------------------------------

const float kPitchGatherRange = 24.0f;	// all cars this close to the middle
const float kPitchParkedSpeed = 1.5f * METERS_PER_SECOND_TO_GAME_SPEED;
const float kPitchRadius = 14.0f;	// two-goal pitch: each goal this far out
const float kPitchTriangleRadius = 13.0f;	// three-goal pitch: vertex radius
const float kPitchGoalRadius = 4.5f;
const int kPitchRingMarks = 16;		// the mission ring round the pitch
const float kPitchRingRadius = 19.5f;	// outside both goal layouts
const float kPitchTouchRange = 4.0f;	// last touch is the nearest car this close
const float kPitchClearance = 4.0f;	// ball spot clear of the cars
// The ring is not decoration: a ball whose centre crosses it is out, and is
// put back at the middle.  It is the line the players can see.
const float kPitchLeash = kPitchRingRadius;
const uint32 kPitchHoldMs = 500;	// the brake is held this long to start
const uint32 kPitchCountdownMs = 3000;
const uint32 kPitchResetMs = 2000;	// after a goal, before the next one counts
const uint32 kPitchGoalNoteMs = 1500;
const uint32 kPitchFinishMs = 5000;
const uint32 kPitchRearmMs = 10000;
const int kPitchGoalsToWin = 3;

// Caddy football: two goals and 1v1 or 2v2, or three goals in a triangle and
// every player for themselves.  The ball is a Caddy with nobody in it; the
// players' own cars push it.  The pitch is built wherever the party stops, so
// there is no venue to find and no asset to add.
struct PitchState
{
	bool active;
	bool counting;
	bool finished;
	uint32 countdownAt;
	uint32 resetAt;
	uint32 finishAt;
	uint32 rearmAt;
	uint32 brakeSince[NUMPLAYERS];
	bool brakeSpent[NUMPLAYERS];
	CVector centre;
	CVector forward;
	CVector goal[3];
	CVector ring[kPitchRingMarks];
	int goalOwner[3];	// three-goal pitch: which player defends it
	int goalTeam[3];	// two-goal pitch: which team defends it
	int players;		// 2, 3 or 4
	int team[NUMPLAYERS];	// two-goal pitch only
	int score[NUMPLAYERS];	// three-goal pitch only
	int teamScore[2];	// two-goal pitch only
	int lastTouch;
	int lastGoalBy;
	bool lastGoalOwn;
	uint32 lastGoalAt;
	int winner;
	int winnerTeam;
	CVehicle *ball;
};
PitchState s_pitch;

// --- King of the Hill's numbers ----------------------------------------------

const float kKothRadius = 12.0f;
const int kKothRingMarks = 12;
const float kKothGatherRange = 14.0f;	// the ring, plus a couple of metres
const uint32 kKothHoldMs = 600;		// every present player holds aim
const uint32 kKothCountdownMs = 3000;
const uint32 kKothRoundMs = 90000;
const uint32 kKothReviveMs = 4000;	// a downed partner is brought back this soon
const uint32 kKothAloneGraceMs = 6000;	// a revive's gap is not the round ending
const uint32 kKothFinishMs = 5000;
const uint32 kKothRearmMs = 10000;
const int kKothBarCells = 6;		// a cell every 15 seconds
const int kKothPay = 500;

// King of the Hill: a ring at the party's feet.  Exactly one player inside
// scores while they are the only one; two or more contest it and nobody
// scores.  The player inside can be on foot, in a car or in a Rhino; damage
// is forced on for the round, whatever the friendly-fire toggle says.
struct KothState
{
	bool active;
	bool counting;
	bool finished;
	uint32 countdownAt;
	uint32 endAt;
	uint32 finishAt;
	uint32 rearmAt;
	uint32 groupSince;	// when every present player began holding aim
	bool groupSpent;	// that hold already started a round; release to ask again
	bool groupAll;		// every present player is holding it now
	int groupHold;		// how many are holding it now, for the other modes
	CVector centre;
	CVector ring[kKothRingMarks];
	uint32 scoreMs[NUMPLAYERS];
	uint32 lastScoreAt;
	uint32 downSince[NUMPLAYERS];
	uint32 aloneSince;
	int holder;
	int inside;
	int winner;
};
KothState s_koth;

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
const float kDanceFloor = 8.0f;		// the stage ring, on the leave line
const int kDanceRingMarks = 12;		// three marks to each player's colour
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
	CVector ring[kDanceRingMarks];
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
const int kCnRRingMarks = 12;		// the green mission ring round the getaway
const float kCnRBustRange = 5.0f;
const uint32 kCnRBustHoldMs = 1500;
const uint32 kCnRBustLockMs = 2000;	// a busted carrier may not take the case back for this long
const uint32 kCnRGraceMs = 2000;	// ...and whoever picks it up cannot be busted for this long
const uint32 kCnRHoldMs = 600;		// the nod is held this long to start a match, not tapped
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
	CVector getawayRing[kCnRRingMarks];
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
	// The other half of the same fix: whoever takes the case up is out of
	// reach of a bust for kCnRGraceMs, so the buster -- who is almost always
	// the one who picks it up -- is not busted by the player they just busted
	// 1.5 s later, and a pair glued together cannot trade the case, a point
	// each time, at the speed of the hold.
	uint32 graceUntil;
	int noteBuster;
	int noteBusted;
	uint32 noteAt;
	uint32 outSince;
	int winner;
	int32 caseBlip;
	int32 getawayBlip;
	// The nod that starts a match: when each player's press began (0 for not
	// down), and whether the press is one that cannot start anything -- it
	// began with a car in reach, or while a match or another mode was on, or
	// it has already been tried -- and so counts for nothing until it is let go.
	uint32 nodSince[NUMPLAYERS];
	bool nodSpent[NUMPLAYERS];
};
CnRState s_cnr;

// --- menu quick starts -------------------------------------------------------
// The menu's versions of the modes need a clear, known piece of ground.  The
// airport apron is the largest flat space on the map and carries the three
// vehicle modes; the beaches are flat enough for the foot modes, and the two
// road/water starts are picked from the path graph below.
struct MenuVenue
{
	CVector pos;
	float heading;
};
const MenuVenue kAirportMenu = { CVector(-1540.0f, -1110.0f, 14.0f), 0.0f };
const MenuVenue kKothMenu = { CVector(700.0f, 1000.0f, 20.0f), 0.0f };
const MenuVenue kDanceMenu = { CVector(700.0f, -1450.0f, 10.0f), 0.0f };
const MenuVenue kCnRMenu = { CVector(650.0f, -1250.0f, 10.0f), 0.0f };
const CVector kShiftMenuBase(650.0f, -1250.0f, 10.0f);
const CVector kRunMenuBase(-850.0f, -1250.0f, 0.0f);

struct MenuPending
{
	int mode;
	uint32 requestedAt;
};
MenuPending s_menuPending = { -1, 0 };

void ReleaseLoaner(int player);

// --- what a mode tells the party when it arms --------------------------------

// Every mode starts from something the party does in passing -- a honk, a pair
// of jumps, a held press of the nod -- and the help box's mode name is all the game
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
	EXPLAIN_PITCH,
	EXPLAIN_KOTH,
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
	return s_shift.active || s_run.active || s_derby.active || s_rc.active || s_dance.active || s_cnr.active ||
	       s_pitch.active || s_koth.active;
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

// Whether anyone in the party is wanted.  The heat is the party's: the level the
// police read is player 1's, and the partners carry it with them, but a partner
// on a record of their own is as much the party's problem, so everyone in the
// world is asked.  The toy race, the dance-off and cops & robbers all start
// from something the party does in passing, and a party with the police after
// it is doing it for another reason -- a jump to clear a fence, a duck behind a
// car, a press of the nod at a door.  The cop shift and the smuggling run are
// not held back by this: the police are what they are about.
bool
PartyWanted(void)
{
	for(int player = 0; player < NUMPLAYERS; player++){
		CPlayerPed *ped = CCoop::GetPlayerPed(player);
		if(ped != nil && ped->m_pWanted != nil && ped->m_pWanted->GetWantedLevel() > 0)
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
	// The menu's police car goes back to the traffic system with the shift.
	for(int i = 0; i < NUMPLAYERS; i++)
		ReleaseLoaner(i);
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

// The boat player 1 is driving, if it is one a run can be sailed in.  The
// skipper is always player 1, because the world is streamed round player 1 and
// the whole party is held to them.  Nobody else can be aboard: a boat has no
// door frames, so the game gives it one passenger seat at most, and that seat
// is on top of the driver's (see CCoopModes::EscortBoat).  The Skimmer is a
// boat that flies, and holding it "slow in the ring" means nothing.  Only the
// arming uses this; the run itself holds its own boat.
CVehicle *
FindLeadBoat(void)
{
	CPlayerPed *ped = CCoop::GetPlayerPed(0);
	if(ped == nil || ped->DyingOrDead() || !ped->bInVehicle || ped->m_pMyVehicle == nil)
		return nil;
	CVehicle *veh = ped->m_pMyVehicle;
	if(veh->GetVehicleAppearance() != VEHICLE_APPEARANCE_BOAT || veh->GetModelIndex() == MI_SKIMMER)
		return nil;
	if(veh->pDriver != ped || veh->GetStatus() == STATUS_WRECKED)
		return nil;
	return veh;
}

// The boat a partner is driving beside the lead, or nil.  Whether it is one the
// run made for them or one of their own makes no difference: the run starts
// when player 1 honks, and a partner may already be at the wheel.
CVehicle *
PartnerBoat(int player, CVehicle *lead)
{
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	if(ped == nil || !ped->bInVehicle || ped->m_pMyVehicle == nil || ped->m_pMyVehicle == lead)
		return nil;
	CVehicle *veh = ped->m_pMyVehicle;
	if(!veh->IsBoat() || veh->pDriver != ped || veh->GetStatus() == STATUS_WRECKED)
		return nil;
	return veh;
}

CVehicle *
BoatOfPlayer(int player, CVehicle *lead)
{
	if(player == 0)
		return lead;
	return PartnerBoat(player, lead);
}

bool
BoatInDropRing(CVehicle *boat)
{
	if(boat == nil)
		return false;
	return (boat->GetPosition() - s_run.drop).Magnitude2D() < kDropRadius &&
	       boat->GetMoveSpeed().Magnitude() < kDropSpeed;
}

// The crew apart from the skipper: boats a partner is driving, near the lead
// boat.
int
CountEscorts(CVehicle *lead)
{
	int count = 0;
	for(int player = 1; player < NUMPLAYERS; player++){
		CVehicle *veh = PartnerBoat(player, lead);
		if(veh != nil && (veh->GetPosition() - lead->GetPosition()).Magnitude() <= kEscortRange)
			count++;
	}
	return count;
}

// Water for one partner's boat: to a side of the lead boat and astern of it,
// further out for each partner, then the other side, then straight astern.  It
// has to be water -- the same test the regroup uses for a boat it puts back --
// and clear of anything standing at the surface, a pier or a moored boat, the
// lead boat and the partners' own included; a spot that fails either is not
// used, and a partner with none waits as they would for any boat ride.  The
// test is a sphere that reaches two metres under the surface, no deeper: a boat
// is not turned away from the shallows of a bay, only from what it would hit.
// Nothing is ever cleared from the spot (CTheScripts::ClearSpaceForMissionEntity
// deletes the vehicles it finds there, and the lead boat is one).
bool
FindEscortSpot(int player, CVehicle *lead, CVector &out)
{
	CVector right = lead->GetRight();
	CVector forward = lead->GetForward();
	right.z = 0.0f;
	forward.z = 0.0f;
	if(right.Magnitude() < 0.1f || forward.Magnitude() < 0.1f){
		right = CVector(1.0f, 0.0f, 0.0f);
		forward = CVector(0.0f, 1.0f, 0.0f);
	}
	right.Normalise();
	forward.Normalise();

	const float lane = (float)(player - 1);
	const float side = (player & 1) ? 1.0f : -1.0f;
	const float wide = kEscortGap * (1.0f + 0.8f*(float)((player - 1)/2));
	const float back = kEscortBack + 6.0f*lane;
	const float offsets[4][2] = {
		{ side*wide, back },
		{ -side*wide, back },
		{ 0.0f, back + kEscortGap },
		{ 0.0f, back + 2.0f*kEscortGap },
	};
	for(int i = 0; i < 4; i++){
		CVector pos = lead->GetPosition() + right*offsets[i][0] - forward*offsets[i][1];
		float level;
		if(!CWaterLevel::GetWaterLevel(pos, &level, true))
			continue;
		pos.z = level;
		if(CWorld::TestSphereAgainstWorld(CVector(pos.x, pos.y, level + 2.0f), 4.0f, nil, true, true, false, true, false, false) != nil)
			continue;
		out = pos;
		return true;
	}
	return false;
}

// Lets go of a partner's boat the way the world takes back the Predators: it
// becomes any other boat on the water.  Never deleted here, so nobody sitting
// in it goes with it.
void
ReleaseEscort(int player)
{
	CVehicle *boat = s_run.escort[player];
	if(boat != nil){
		CTheScripts::CleanUpThisVehicle(boat);
		boat->CleanUpOldReference((CEntity**)&s_run.escort[player]);
		s_run.escort[player] = nil;
	}
}

// The end of the run: the party's boats are handed back.  A partner at the
// wheel of one is taken out of the world first -- CCoop::Suspend, the same
// door a change of clothes uses -- and comes back by the usual rules: beside
// player 1 on foot, or after the boat ride if player 1 is still sailing.  Left
// where they were they would be stuck at the wheel of a boat at sea, which has
// no exit button for them while it floats.
void
ReleaseEscorts(void)
{
	bool crewed = false;
	for(int player = 1; player < NUMPLAYERS; player++)
		if(s_run.escort[player] != nil && s_run.escort[player]->pDriver != nil)
			crewed = true;
	if(crewed)
		CCoop::Suspend("smuggling run over");
	for(int player = 1; player < NUMPLAYERS; player++)
		ReleaseEscort(player);
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

// The boat carrying the crate, on the radar in the game's green.  Before a
// pickup the crate itself is the yellow coordinate marker; once it is aboard,
// this is the boat the other one has to stay with.
void
ClearObjectiveMarker(void)
{
	if(s_run.objectiveBlip >= 0){
		CRadar::ClearBlip(s_run.objectiveBlip);
		s_run.objectiveBlip = -1;
	}
	s_run.objectiveHandle = -1;
}

void
UpdateObjectiveMarker(CVehicle *boat)
{
	const int32 handle = boat != nil ? CPools::GetVehiclePool()->GetIndex(boat) : -1;
	if(handle == s_run.objectiveHandle && s_run.objectiveBlip >= 0)
		return;
	ClearObjectiveMarker();
	if(handle < 0)
		return;
	s_run.objectiveBlip = CRadar::SetEntityBlip(BLIP_CAR, handle,
		RADAR_TRACE_GREEN, BLIP_DISPLAY_BOTH);
	CRadar::ChangeBlipColour(s_run.objectiveBlip, RADAR_TRACE_GREEN);
	s_run.objectiveHandle = handle;
}

// The crate, before either boat has it.  A coordinate blip is enough on the
// radar; the corona is what makes it findable in the water ahead.
void
ClearCargoMarker(void)
{
	if(s_run.cargoBlip >= 0){
		CRadar::ClearBlip(s_run.cargoBlip);
		s_run.cargoBlip = -1;
	}
}

void
UpdateCargoMarker(bool show)
{
	if(!show){
		ClearCargoMarker();
		return;
	}
	if(s_run.cargoBlip < 0){
		s_run.cargoBlip = CRadar::SetCoordBlip(BLIP_COORD, s_run.cargoPos[s_run.leg],
			RADAR_TRACE_YELLOW, BLIP_DISPLAY_BOTH);
		CRadar::ChangeBlipColour(s_run.cargoBlip, RADAR_TRACE_YELLOW);
	}
}

void
RegisterCargoCorona(void)
{
	CCoronas::RegisterCorona((uintptr)&s_run + 2,
		255, 220, 90, 255,
		s_run.cargoPos[s_run.leg], 4.0f, 500.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
		CCoronas::FLARE_NONE, CCoronas::REFLECTION_ON,
		CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
}

void
RegisterDropCorona(void)
{
	CCoronas::RegisterCorona((uintptr)&s_run,
		255, 170, 70, 255,
		s_run.drop, 5.0f, 500.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
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
	predator->SetOrientation(0.0f, 0.0f, boat->GetForward().Heading());
	predator->GetMatrix().UpdateRW();
	predator->UpdateRwFrame();
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

void ReleaseLoaner(int player);

// The crate waits about ninety metres short of the drop, on the line from the
// middle of the two boats: neither starts with it in their lap, and both can
// steer for it.  It is planned when a leg actually begins, because on the menu
// start the escort boats do not exist yet when the run arms.
void
PlanCargoForLeg(CVehicle *boat)
{
	CVector start = boat->GetPosition();
	int partners = 0;
	for(int player = 1; player < NUMPLAYERS; player++){
		CVehicle *escort = PartnerBoat(player, boat);
		if(escort != nil){
			start += escort->GetPosition();
			partners++;
		}
	}
	if(partners > 0)
		start /= (float)(partners + 1);

	CVector dir = s_run.dropPos[s_run.leg] - start;
	dir.z = 0.0f;
	if(dir.Magnitude() < 1.0f)
		dir = CVector(1.0f, 0.0f, 0.0f);
	dir.Normalise();
	if(!FindWaterNodeNear(s_run.dropPos[s_run.leg] - dir*kCargoDistance, kCargoSearch,
	                      s_run.cargoPos[s_run.leg]))
		s_run.cargoPos[s_run.leg] = s_run.dropPos[s_run.leg];
}

bool
StartRunAt(uint32 now, CVehicle *boat)
{
	if(boat == nil || !PlanRoute(boat))
		return false;

	ClearObjectiveMarker();
	ClearCargoMarker();
	CStreaming::RequestModel(MI_PREDATOR, 0);
	CStreaming::RequestModel(MI_COP, 0);

	boat->RegisterReference((CEntity**)&s_run.boat);
	s_run.boat = boat;
	s_run.active = true;
	s_run.leg = 0;
	s_run.cargoPlanned = false;
	s_run.firstHeat = false;
	s_run.carrier = -1;
	s_run.drop = s_run.dropPos[0];
	s_run.unloadSince = 0;
	s_run.unloadBoat = -1;
	s_run.crewLostAt = 0;
	s_run.nextRetargetAt = now;
	PlaceDropMarker();
	COOP_LOG("WII coop: smuggling run on\n");
	CCoop::Notify("WII_CSR");
	ShowExplainer(EXPLAIN_RUN, now);
	return true;
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
	// Player 1 at the wheel of a boat and the horn: the rest of the party is
	// given boats of their own beside it as they come into the world, by
	// CCoopModes::EscortBoat.
	CVehicle *boat = FindLeadBoat();
	if(boat == nil)
		return;
	CPad *pad = GetPadFromVehicleDriver(boat);
	if(pad == nil || !pad->GetHorn())
		return;
	StartRunAt(now, boat);
}

void
EndRun(uint32 now, const char *why)
{
	ClearDropMarker();
	ClearObjectiveMarker();
	ClearCargoMarker();
	ReleasePursuers();
	ReleaseEscorts();
	if(s_run.boat != nil){
		s_run.boat->CleanUpOldReference((CEntity**)&s_run.boat);
		s_run.boat = nil;
	}
	// A lead boat the menu made is a loaner and goes back with the run; a
	// player's own boat was never in this slot and is left alone.
	ReleaseLoaner(0);
	s_run.active = false;
	s_run.rearmAt = now + kRearmMs;
	s_run.unloadSince = 0;
	s_run.crewLostAt = 0;
	COOP_LOG("WII coop: smuggling run off (%s)\n", why);
}

void
CompleteDrop(uint32 now, CVehicle *boat)
{
	// The pay is the party's, and the boats that kept up are what earn it.
	CWorld::Players[CWorld::PlayerInFocus].m_nMoney += kDropPay + kEscortPay*CountEscorts(boat);
	s_run.carrier = -1;
	s_run.unloadBoat = -1;
	ClearObjectiveMarker();
	ClearCargoMarker();
	CPlayerPed *lead = FindPlayerPed();
	if(lead != nil && lead->m_pWanted != nil && lead->m_pWanted->GetWantedLevel() < 3)
		lead->m_pWanted->SetWantedLevel(lead->m_pWanted->GetWantedLevel() + 1);

	s_run.leg++;
	s_run.cargoPlanned = false;
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

	// The lead boat is the run: while it exists and its skipper is in it the run
	// goes on; lost, wrecked, or left empty for a moment and it is over.  The
	// partners' boats are not the run's to keep -- a partner who is down or has
	// left is simply not in the crew -- and are handed back with it.
	CVehicle *boat = s_run.boat;
	if(boat == nil){
		EndRun(now, "boat lost");
		return;
	}
	if(boat->GetStatus() == STATUS_WRECKED){
		EndRun(now, "boat wrecked");
		return;
	}

	CPlayerPed *skipper = CCoop::GetPlayerPed(0);
	if(skipper == nil || !skipper->bInVehicle || skipper->m_pMyVehicle != boat){
		if(s_run.crewLostAt == 0){
			s_run.crewLostAt = now;
		}else if(now - s_run.crewLostAt >= kCrewGraceMs){
			EndRun(now, "skipper ashore");
			return;
		}
		// A halt while nobody is aboard does not keep counting towards an
		// unload.
		s_run.unloadSince = 0;
		UpdatePursuers(now, boat);
		return;
	}
	s_run.crewLostAt = 0;

	// One Predator from the first leg: player 1 has heat on the way to the
	// crate, so player 2 has something to cover from the start.  After this the
	// drops bring the rest of the heat.
	if(!s_run.firstHeat && s_run.leg == 0){
		SpawnPursuer(now, boat, 0);
		if(s_run.pursuer[0] != nil)
			s_run.firstHeat = true;
	}

	if(!s_run.cargoPlanned){
		PlanCargoForLeg(boat);
		s_run.cargoPlanned = true;
		ClearCargoMarker();
	}

	// The cargo: both boats race for the crate; the first within reach carries
	// it, and the cops chase that boat.  If it is lost -- the carrier's boat is
	// wrecked or left -- it floats where it was and the other boat can take it.
	CVehicle *carrierBoat = nil;
	if(s_run.carrier >= 0){
		carrierBoat = BoatOfPlayer(s_run.carrier, boat);
		if(carrierBoat == nil){
			s_run.carrier = -1;
			s_run.unloadSince = 0;
			s_run.unloadBoat = -1;
			ClearObjectiveMarker();
			ClearCargoMarker();
		}else{
			// Remember where it would be dropped if this boat goes.
			s_run.cargoPos[s_run.leg] = carrierBoat->GetPosition();
		}
	}
	if(s_run.carrier < 0){
		int bestPlayer = -1;
		float bestDist = kCargoRadius;
		for(int player = 0; player < NUMPLAYERS; player++){
			CVehicle *candidate = BoatOfPlayer(player, boat);
			if(candidate == nil)
				continue;
			const float dist = (candidate->GetPosition() - s_run.cargoPos[s_run.leg]).Magnitude2D();
			if(dist < bestDist){
				bestDist = dist;
				bestPlayer = player;
			}
		}
		if(bestPlayer >= 0){
			s_run.carrier = bestPlayer;
			carrierBoat = BoatOfPlayer(bestPlayer, boat);
			s_run.unloadSince = 0;
			s_run.unloadBoat = -1;
			ClearCargoMarker();
			COOP_LOG("WII coop: player %d has the cargo\n", bestPlayer + 1);
		}
	}

	UpdateObjectiveMarker(carrierBoat);
	UpdateCargoMarker(s_run.carrier < 0);
	if(s_run.carrier < 0)
		RegisterCargoCorona();

	// Only the boat carrying the crate unloads it, and it has to hold still in
	// the ring.  The other boat's job is to keep the police off it.
	if(carrierBoat != nil){
		const int32 handle = CPools::GetVehiclePool()->GetIndex(carrierBoat);
		if(handle != s_run.unloadBoat){
			s_run.unloadBoat = handle;
			s_run.unloadSince = 0;
		}
		if(BoatInDropRing(carrierBoat)){
			if(s_run.unloadSince == 0)
				s_run.unloadSince = now;
			else if(now - s_run.unloadSince >= kUnloadMs){
				CompleteDrop(now, boat);
				return;
			}
		}else{
			s_run.unloadSince = 0;
		}
	}else{
		s_run.unloadBoat = -1;
		s_run.unloadSince = 0;
	}

	UpdatePursuers(now, carrierBoat != nil ? carrierBoat : boat);
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

// A mission-style ring on the ground: a circle of coronas marking a pitch,
// a stage or a drop-off, the way the derby and the hill already mark theirs.
// The caller owns the key block; ids below it are for the mode's other marks.
void
RegisterGroundRing(uintptr key, const CVector *points, int count, CRGBA colour, float size)
{
	for(int i = 0; i < count; i++)
		CCoronas::RegisterCorona(key + i, colour.r, colour.g, colour.b, 255,
			points[i], size, 250.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
			CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
			CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
}

// Lets go of a player's loaner: the world takes it back, like the suspect and
// the Predators.
void
ReleaseLoaner(int player)
{
	CVehicle *car = s_derby.loaner[player];
	if(car != nil){
		CTheScripts::CleanUpThisVehicle(car);
		if(car->bIsLawEnforcer)
			car->ChangeLawEnforcerState(false);
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
	car->GetMatrix().UpdateRW();
	car->UpdateRwFrame();
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

bool PlayerInPitchCar(int player, CVehicle **out);

// Puts a player at the wheel of a car the menu made.  This is the same order
// and warp the game's own warp-player-into-car command uses; WarpPedIntoCar
// only sits them down if the enter objective took first.
bool
SeatMenuDriver(CPlayerPed *ped, CVehicle *car)
{
	if(ped == nil || car == nil || car->pDriver != nil || ped->bInVehicle)
		return false;
	ped->SetPedState(PED_IDLE);
	ped->SetMoveState(PEDMOVE_STILL);
	ped->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
	ped->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, car);
	if(ped->m_objective != OBJECTIVE_ENTER_CAR_AS_DRIVER)
		return false;
	ped->WarpPedIntoCar(car);
	ped->SetObjective(OBJECTIVE_NONE);
	return ped->bInVehicle && car->pDriver == ped;
}

CVector
MenuGround(const CVector &point, float lift)
{
	CVector out = point;
	bool found = false;
	const float ground = CWorld::FindGroundZFor3DCoord(point.x, point.y, point.z + 5.0f, &found);
	out.z = (found ? ground : point.z) + lift;
	return out;
}

// The menu teleports across the map, so the destination's collision has to be
// there before anybody is put down on it.  This is the blocking pair the
// shortcut taxi uses; the frame is already past the world update.
void
MenuLoadScene(const CVector &at)
{
	CStreaming::LoadSceneCollision(at);
	CStreaming::LoadScene(at);
}

// The scene switch drops models that are merely requested, so a mode's own
// car or boat is asked for again after the collision is in place and loaded
// before anything is made out of it.
bool
MenuEnsureModel(int model)
{
	CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
	CStreaming::LoadAllRequestedModels(false);
	return CStreaming::HasModelLoaded(model);
}

bool
MenuVenueOnLand(const MenuVenue &venue, CVector *out)
{
	bool found = false;
	const float ground = CWorld::FindGroundZFor3DCoord(venue.pos.x, venue.pos.y,
	                                                  venue.pos.z + 10.0f, &found);
	if(!found)
		return false;
	float water = 0.0f;
	if(CWaterLevel::GetWaterLevel(venue.pos, &water, true) && ground < water + 0.5f)
		return false;
	if(out != nil){
		*out = venue.pos;
		out->z = ground;
	}
	return true;
}

CVector
MenuSlot(const MenuVenue &venue, int player, float gap)
{
	const CVector forward(-Sin(venue.heading), Cos(venue.heading), 0.0f);
	const CVector right(Cos(venue.heading), Sin(venue.heading), 0.0f);
	const float side = (player & 1) ? gap : -gap;
	const float ahead = (player & 2) ? gap : -gap;
	return venue.pos + right*side + forward*ahead;
}

// Leaves whatever the player is in the way an ordinary exit does, without
// playing the door animation: the vehicle is not being kept, and the ped is
// about to be teleported anyway.  PedSetOutCarCB is the engine's own final
// step; the manual clear afterwards is only because it leaves m_pMyVehicle
// behind for its caller to nil.
void
MenuLeaveVehicle(CPlayerPed *ped)
{
	if(ped == nil || !ped->bInVehicle || ped->m_pMyVehicle == nil)
		return;
	CVehicle *vehicle = ped->m_pMyVehicle;
	ped->SetPedState(PED_EXIT_CAR);
	ped->RemoveInCarAnims();
	CPed::PedSetOutCarCB(nil, ped);
	if(ped->m_pMyVehicle != nil){
		ped->m_pMyVehicle->CleanUpOldReference((CEntity**)&ped->m_pMyVehicle);
		ped->m_pMyVehicle = nil;
	}
	ped->bInVehicle = false;
	ped->bUsesCollision = true;
	ped->SetPedState(PED_IDLE);
	ped->SetMoveState(PEDMOVE_STILL);
	if(vehicle != nil && vehicle->pDriver == nil && vehicle->GetStatus() != STATUS_WRECKED)
		vehicle->SetStatus(STATUS_ABANDONED);
}

void
MenuPlacePed(CPlayerPed *ped, const CVector &at, float heading)
{
	CVector pos = at;
	if(!CPedPlacement::FindZCoorForPed(&pos))
		pos = MenuGround(at, 0.0f);
	ped->Teleport(pos);
	ped->m_fRotationCur = heading;
	ped->m_fRotationDest = heading;
	ped->SetHeading(heading);
	ped->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
	ped->bIsStanding = true;
}

void
MenuPlaceCar(CVehicle *car, const CVector &at, float heading)
{
	CVector pos = MenuGround(at, 0.0f);
	pos.z += car->GetDistanceFromCentreOfMassToBaseOfModel();
	car->Teleport(pos);
	car->SetOrientation(0.0f, 0.0f, heading);
	car->GetMatrix().UpdateRW();
	car->UpdateRwFrame();
	car->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
	car->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
	car->bIsHandbrakeOn = true;
	car->bEngineOn = true;
}

bool
MenuStoreLoaner(CVehicle *vehicle)
{
	if(vehicle == nil)
		return false;
	for(int i = 0; i < NUMPLAYERS; i++){
		if(s_derby.loaner[i] == nil){
			vehicle->RegisterReference((CEntity**)&s_derby.loaner[i]);
			s_derby.loaner[i] = vehicle;
			return true;
		}
	}
	return false;
}

bool
MenuSeatPassenger(CPlayerPed *ped, CVehicle *car)
{
	if(ped == nil || car == nil || car->pDriver == nil || ped->bInVehicle)
		return false;
	ped->SetPedState(PED_IDLE);
	ped->SetMoveState(PEDMOVE_STILL);
	for(int i = 0; i < car->m_nNumMaxPassengers; i++){
		if(car->pPassengers[i] != nil)
			continue;
		if(!car->AddPassenger(ped, i))
			return false;
		ped->m_pMyVehicle = car;
		ped->m_pMyVehicle->RegisterReference((CEntity**)&ped->m_pMyVehicle);
		ped->bInVehicle = true;
		ped->SetPedState(PED_DRIVING);
		ped->bUsesCollision = false;
		ped->AddInCarAnims(car, false);
		return true;
	}
	return false;
}

// The foot modes start in a loose square.  The beach is wide enough for the
// rings the modes draw and close enough that the menu's own start checks pass.
bool
MenuPrepareFoot(const MenuVenue &venue)
{
	MenuLoadScene(venue.pos);
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(ped->bInVehicle)
			MenuLeaveVehicle(ped);
		MenuPlacePed(ped, MenuSlot(venue, i, 4.0f), venue.heading);
	}
	CCoop::ClearTeleportWatch();
	return true;
}

// Caddy football and the derby need everyone at a wheel.  Existing eligible
// cars are brought along; anyone else leaves what they are in and gets a
// Bloodring loaner at the venue.  This is the "if they are not in that
// vehicle, put them in one" half of the menu.
bool
MenuPrepareCars(const MenuVenue &venue, bool caddy)
{
	MenuLoadScene(venue.pos);
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		CVehicle *car = nil;
		const bool ready = caddy ? PlayerInPitchCar(i, &car) : PlayerInDerbyCar(i, &car);
		if(ready && car != nil && car->GetStatus() != STATUS_WRECKED){
			MenuPlaceCar(car, MenuSlot(venue, i, 8.0f), venue.heading);
			continue;
		}
		if(ped->bInVehicle)
			MenuLeaveVehicle(ped);
		MenuPlacePed(ped, MenuSlot(venue, i, 8.0f), venue.heading);
	}

	bool loanerModelReady = false;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil || ped->bInVehicle)
			continue;
		if(!loanerModelReady){
			if(!MenuEnsureModel(MI_BLOODRA))
				return false;
			loanerModelReady = true;
		}
		SpawnLoaner(i);
		CVehicle *car = s_derby.loaner[i];
		if(car == nil || !SeatMenuDriver(ped, car))
			return false;
	}
	CCoop::ClearTeleportWatch();
	return true;
}

void
ReleaseMenuLoaners(void)
{
	for(int i = 0; i < NUMPLAYERS; i++)
		ReleaseLoaner(i);
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

// --- Caddy football ----------------------------------------------------------

// A player's own car, driven by them, that is not a law car and not one of the
// heavy things that would turn the Caddy into a pancake.
bool
PlayerInPitchCar(int player, CVehicle **out)
{
	if(!PlayerInDerbyCar(player, out))
		return false;
	switch((*out)->GetModelIndex()){
	case MI_RHINO:
	case MI_FIRETRUCK:
	case MI_AMBULAN:
	case MI_ENFORCER:
	case MI_BARRACKS:
		return false;
	default:
		return true;
	}
}

// A spot near the middle that is not inside a player's car.  The party arms
// from a loose group, so the middle is usually clear; this only matters when
// they stop almost on top of each other.
CVector
PitchBallSpot(void)
{
	const CVector right(s_pitch.forward.y, -s_pitch.forward.x, 0.0f);
	CVector spot = s_pitch.centre;
	// Seven tries: the middle, then five, ten and fifteen metres either side.
	// The last is still inside the ring; a twentieth metre would spawn the
	// ball outside the out-of-play line and reset it forever.
	for(int attempt = 0; attempt < 7; attempt++){
		if(attempt != 0){
			const int step = (attempt + 1)/2;
			const float offset = (attempt & 1 ? 1.0f : -1.0f)*(float)step*(kPitchClearance + 1.0f);
			spot = s_pitch.centre + right*offset;
		}
		bool clear = true;
		for(int i = 0; i < NUMPLAYERS; i++){
			if(CCoop::GetPlayerPed(i) == nil)
				continue;
			if((PlayerCoors(i) - spot).Magnitude2D() < kPitchClearance){
				clear = false;
				break;
			}
		}
		if(clear)
			break;
	}
	return spot;
}

void
ReleasePitchBall(void)
{
	if(s_pitch.ball != nil){
		CTheScripts::CleanUpThisVehicle(s_pitch.ball);
		s_pitch.ball->CleanUpOldReference((CEntity**)&s_pitch.ball);
		s_pitch.ball = nil;
	}
}

// The Caddy: a mission vehicle with nobody in it, locked so the world leaves
// it alone, bulletproof and fireproof so a stray shot or a hot exhaust does
// not end the match, and handbrake off so it rolls when it is hit.
bool
SpawnPitchBall(void)
{
	if(s_pitch.ball != nil)
		return true;
	if(!CStreaming::HasModelLoaded(MI_CADDY))
		return false;

	CVector pos = PitchBallSpot();
	bool found = false;
	const float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
	if(found)
		pos.z = ground;

	CAutomobile *ball = new CAutomobile(MI_CADDY, MISSION_VEHICLE);
	if(ball == nil)
		return false;
	pos.z += ball->GetDistanceFromCentreOfMassToBaseOfModel();
	ball->SetPosition(pos);
	ball->SetOrientation(0.0f, 0.0f, s_pitch.forward.Heading());
	CTheScripts::ClearSpaceForMissionEntity(pos, ball);
	ball->GetMatrix().UpdateRW();
	ball->UpdateRwFrame();
	ball->SetStatus(STATUS_ABANDONED);
	ball->bIsLocked = true;
	ball->m_nDoorLock = CARLOCK_LOCKED;
	ball->bEngineOn = false;
	ball->bIsHandbrakeOn = false;
	ball->bBulletProof = true;
	ball->bFireProof = true;
	ball->m_fHealth = 1000.0f;
	ball->AutoPilot.m_nCarMission = MISSION_NONE;
	ball->AutoPilot.m_nTempAction = TEMPACT_NONE;
	ball->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	CWorld::Add(ball);
	ball->RegisterReference((CEntity**)&s_pitch.ball);
	s_pitch.ball = ball;
	return true;
}

void
ResetPitchBall(void)
{
	if(s_pitch.ball == nil || s_pitch.ball->GetStatus() == STATUS_WRECKED){
		ReleasePitchBall();
		SpawnPitchBall();
		return;
	}
	CVector pos = PitchBallSpot();
	bool found = false;
	const float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
	if(found)
		pos.z = ground;
	s_pitch.ball->SetPosition(pos);
	s_pitch.ball->SetMoveSpeed(0.0f, 0.0f, 0.0f);
	s_pitch.ball->SetTurnSpeed(0.0f, 0.0f, 0.0f);
	s_pitch.ball->SetOrientation(0.0f, 0.0f, s_pitch.forward.Heading());
	s_pitch.ball->GetMatrix().UpdateRW();
	s_pitch.ball->UpdateRwFrame();
	s_pitch.ball->SetStatus(STATUS_ABANDONED);
	s_pitch.ball->bIsHandbrakeOn = false;
	s_pitch.ball->bEngineOn = false;
	// A ball that has taken a few hits but is not wrecked comes back whole:
	// the reset is a fresh ball, not one hit from a wreck.
	s_pitch.ball->m_fHealth = 1000.0f;
}

void
RegisterPitchCoronas(void)
{
	RegisterGroundRing((uintptr)&s_pitch + 8, s_pitch.ring, kPitchRingMarks,
		CRGBA(255, 225, 130, 255), 2.2f);

	const int goals = s_pitch.players == 3 ? 3 : 2;
	for(int g = 0; g < goals; g++){
		CRGBA colour(255, 255, 255, 255);
		if(s_pitch.players == 3 && s_pitch.goalOwner[g] >= 0)
			colour = kCoopColours[s_pitch.goalOwner[g]];
		else if(s_pitch.players != 3 && g < 2)
			colour = kCoopColours[g];
		CCoronas::RegisterCorona((uintptr)&s_pitch + g, colour.r, colour.g, colour.b, 255,
			s_pitch.goal[g], kPitchGoalRadius, 250.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
			CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
			CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
	}
}

void
FinishPitch(uint32 now, int winner, int winnerTeam)
{
	s_pitch.finished = true;
	s_pitch.finishAt = now + kPitchFinishMs;
	s_pitch.winner = winner;
	s_pitch.winnerTeam = winnerTeam;
	// A team whose last player left before the whistle has no one to pay.
	if(winner >= 0)
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += 500;
	COOP_LOG("WII coop: caddy football to %s %d\n", winnerTeam >= 0 ? "team" : "player", winnerTeam >= 0 ? winnerTeam + 1 : winner + 1);
}

void
StartPitch(uint32 now, const CVector &centre, const CVector &forward, int players)
{
	if(s_pitch.ball != nil)
		ReleasePitchBall();

	s_pitch.active = true;
	s_pitch.counting = true;
	s_pitch.finished = false;
	s_pitch.countdownAt = now + kPitchCountdownMs;
	s_pitch.resetAt = 0;
	s_pitch.finishAt = 0;
	s_pitch.rearmAt = 0;
	s_pitch.centre = centre;
	s_pitch.forward = forward;
	s_pitch.players = players;
	s_pitch.lastTouch = -1;
	s_pitch.lastGoalBy = -1;
	s_pitch.lastGoalOwn = false;
	s_pitch.lastGoalAt = 0;
	s_pitch.winner = -1;
	s_pitch.winnerTeam = -1;
	s_pitch.teamScore[0] = 0;
	s_pitch.teamScore[1] = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		s_pitch.brakeSince[i] = 0;
		s_pitch.brakeSpent[i] = false;
		s_pitch.team[i] = players == 3 ? i : i % 2;
		s_pitch.score[i] = 0;
	}
	for(int g = 0; g < 3; g++){
		s_pitch.goalOwner[g] = -1;
		s_pitch.goalTeam[g] = -1;
	}

	if(players == 3){
		// Three goals on a triangle, each player defending the one nearest
		// where they stopped.  Nearest-first, so a loose start still gives
		// everyone one goal and no one two.
		const float heading = forward.Heading();
		for(int g = 0; g < 3; g++){
			const float angle = heading + 2.0f*PI*(float)g/3.0f;
			CVector goal = centre + CVector(Cos(angle), Sin(angle), 0.0f)*kPitchTriangleRadius;
			bool found = false;
			const float ground = CWorld::FindGroundZFor3DCoord(goal.x, goal.y, goal.z + 2.0f, &found);
			goal.z = (found ? ground : centre.z) + 1.0f;
			s_pitch.goal[g] = goal;
		}
		bool assigned[NUMPLAYERS] = { false };
		for(int g = 0; g < 3; g++){
			int best = -1;
			float bestDist = 1000000.0f;
			for(int i = 0; i < NUMPLAYERS; i++){
				if(CCoop::GetPlayerPed(i) == nil || assigned[i])
					continue;
				const float dist = (PlayerCoors(i) - s_pitch.goal[g]).Magnitude2D();
				if(dist < bestDist){
					bestDist = dist;
					best = i;
				}
			}
			if(best >= 0){
				s_pitch.goalOwner[g] = best;
				assigned[best] = true;
			}
		}
	}else{
		// Two goals: team 0 defends the one behind the middle, team 1 the one
		// ahead.
		for(int g = 0; g < 2; g++){
			CVector goal = centre + (g == 0 ? forward*-1.0f : forward)*kPitchRadius;
			bool found = false;
			const float ground = CWorld::FindGroundZFor3DCoord(goal.x, goal.y, goal.z + 2.0f, &found);
			goal.z = (found ? ground : centre.z) + 1.0f;
			s_pitch.goal[g] = goal;
			s_pitch.goalTeam[g] = g;
		}
	}

	// The pitch's own ring, outside both goal layouts: the goals say where
	// to score, this says how far the game reaches.
	for(int i = 0; i < kPitchRingMarks; i++){
		const float angle = 2.0f*PI*(float)i/(float)kPitchRingMarks;
		CVector mark = centre + CVector(Cos(angle), Sin(angle), 0.0f)*kPitchRingRadius;
		bool found = false;
		const float ground = CWorld::FindGroundZFor3DCoord(mark.x, mark.y, mark.z + 2.0f, &found);
		mark.z = (found ? ground : centre.z) + 1.0f;
		s_pitch.ring[i] = mark;
	}

	CStreaming::RequestModel(MI_CADDY, 0);
	COOP_LOG("WII coop: caddy football on (%d players)\n", players);
	CCoop::Notify("WII_CSP");
	ShowExplainer(EXPLAIN_PITCH, now);
}

void
TryStartPitch(uint32 now, bool held)
{
	if(!held)
		return;
	// Every present player holding aim is asking for the hill, not a pitch.
	// One player's brake is the football; a partial group is not a hill ask.
	if(s_koth.groupAll)
		return;
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	if(now < s_pitch.rearmAt)
		return;
	if(PartyWanted())
		return;

	CVehicle *car[NUMPLAYERS];
	CVector sum(0.0f, 0.0f, 0.0f);
	CVector forward(0.0f, 0.0f, 0.0f);
	int count = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		car[i] = nil;
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(ped->DyingOrDead() || ped->m_nPedState == PED_ARRESTED)
			return;
		if(!PlayerInPitchCar(i, &car[i]))
			return;
		sum += car[i]->GetPosition();
		forward += car[i]->GetForward();
		count++;
	}
	if(count < 2)
		return;

	const CVector centre = sum / (float)count;
	for(int i = 0; i < NUMPLAYERS; i++){
		if(car[i] == nil)
			continue;
		if(car[i]->GetMoveSpeed().Magnitude() > kPitchParkedSpeed)
			return;
		if((car[i]->GetPosition() - centre).Magnitude2D() > kPitchGatherRange)
			return;
	}

	forward.z = 0.0f;
	if(forward.Magnitude() < 0.1f)
		forward = CVector(1.0f, 0.0f, 0.0f);
	forward.Normalise();
	StartPitch(now, centre, forward, count);
}

void
EndPitch(uint32 now, const char *why)
{
	ReleasePitchBall();
	// Loaners the menu made for the pitch go back to the world with it.
	for(int i = 0; i < NUMPLAYERS; i++)
		ReleaseLoaner(i);
	s_pitch.active = false;
	s_pitch.counting = false;
	s_pitch.finished = false;
	s_pitch.rearmAt = now + kPitchRearmMs;
	COOP_LOG("WII coop: caddy football off (%s)\n", why);
}

void
UpdatePitch(uint32 now)
{
	// The brake in a car starts it.  The nod is the exit-vehicle button and
	// the horn is the derby's start, so neither can be the football's; the
	// brake is the car's own "we are parked" signal, and the match only starts
	// when every car is stopped anyway.  It is tracked press by press, and a
	// press that began on foot or during another mode counts for nothing
	// until it is let go.
	bool held = false;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil){
			s_pitch.brakeSince[i] = 0;
			s_pitch.brakeSpent[i] = false;
			continue;
		}
		CPad *pad = GetPadFromPlayer(ped);
		if(pad->NewState.RightShoulder1 == 0){
			s_pitch.brakeSince[i] = 0;
			s_pitch.brakeSpent[i] = false;
			continue;
		}
		if(s_pitch.brakeSpent[i])
			continue;
		if(s_pitch.brakeSince[i] == 0){
			if(s_pitch.active || ModeActive() || now < s_pitch.rearmAt || !ped->bInVehicle)
				s_pitch.brakeSpent[i] = true;
			else
				s_pitch.brakeSince[i] = now;
		}else if(now - s_pitch.brakeSince[i] >= kPitchHoldMs){
			held = true;
			s_pitch.brakeSince[i] = 0;
			s_pitch.brakeSpent[i] = true;
		}
	}

	if(!s_pitch.active){
		TryStartPitch(now, held);
		return;
	}
	if(!CCoop::PairActive()){
		EndPitch(now, "session");
		return;
	}
	if(s_pitch.finished){
		if(now >= s_pitch.finishAt)
			EndPitch(now, "over");
		return;
	}

	// Ramming only, as in the derby: the fire button belongs to the match,
	// countdown included.
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil)
			SuppressFire(GetPadFromPlayer(ped));
	}
	// The pitch reads from the countdown on, not only after the first whistle.
	RegisterPitchCoronas();

	if(s_pitch.counting){
		// Hold the lights until the Caddy is actually in the world: the
		// countdown is not worth starting with an empty pitch.
		if(s_pitch.ball == nil){
			SpawnPitchBall();
			s_pitch.countdownAt = now + kPitchCountdownMs;
			return;
		}
		if(now >= s_pitch.countdownAt)
			s_pitch.counting = false;
		return;
	}

	if(s_pitch.ball == nil){
		SpawnPitchBall();
		return;
	}
	if(s_pitch.ball->GetStatus() == STATUS_WRECKED || s_pitch.ball->GetUp().z < 0.3f ||
	   (s_pitch.ball->GetPosition() - s_pitch.centre).Magnitude2D() > kPitchLeash){
		ResetPitchBall();
		s_pitch.lastTouch = -1;
		return;
	}

	// Last touch: the nearest player car within reach of the Caddy.  It is a
	// proximity credit, not a collision callback -- a few lines, and exact
	// enough for who gets the goal.
	CVehicle *ball = s_pitch.ball;
	int best = -1;
	float bestDist = kPitchTouchRange;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil || ped->DyingOrDead() || ped->m_nPedState == PED_ARRESTED)
			continue;
		// The last touch belongs to a driver in an eligible car.  A
		// passenger in a partner's car, or somebody who has climbed into a
		// Rhino or a law car after the start, is not playing the football.
		CVehicle *touchCar = nil;
		if(!PlayerInPitchCar(i, &touchCar))
			continue;
		const float dist = (touchCar->GetPosition() - ball->GetPosition()).Magnitude2D();
		if(dist < bestDist){
			bestDist = dist;
			best = i;
		}
	}
	if(best >= 0)
		s_pitch.lastTouch = best;

	if(now < s_pitch.resetAt){
		RegisterPitchCoronas();
		return;
	}

	const int goals = s_pitch.players == 3 ? 3 : 2;
	for(int g = 0; g < goals; g++){
		if((ball->GetPosition() - s_pitch.goal[g]).Magnitude2D() >= kPitchGoalRadius)
			continue;

		const int shooter = s_pitch.lastTouch;
		if(shooter < 0){
			// Traffic, a kerb or a stray shove rolled it in.  Nobody touched
			// it, so nobody scores; a player can still push it out or in.
			RegisterPitchCoronas();
			return;
		}
		s_pitch.lastGoalAt = now;
		s_pitch.lastGoalBy = shooter;
		if(s_pitch.players == 3){
			const bool own = shooter == s_pitch.goalOwner[g];
			s_pitch.lastGoalOwn = own;
			if(shooter >= 0 && !own){
				s_pitch.score[shooter]++;
				COOP_LOG("WII coop: pitch goal to player %d\n", shooter + 1);
				if(s_pitch.score[shooter] >= kPitchGoalsToWin){
					FinishPitch(now, shooter, -1);
					return;
				}
			}
		}else{
			const int scoringTeam = 1 - s_pitch.goalTeam[g];
			s_pitch.lastGoalOwn = shooter >= 0 && s_pitch.team[shooter] == s_pitch.goalTeam[g];
			s_pitch.teamScore[scoringTeam]++;
			COOP_LOG("WII coop: pitch goal to team %d\n", scoringTeam + 1);
			if(s_pitch.teamScore[scoringTeam] >= kPitchGoalsToWin){
				int winner = -1;
				for(int i = 0; i < NUMPLAYERS; i++)
					if(CCoop::GetPlayerPed(i) != nil && s_pitch.team[i] == scoringTeam){
						winner = i;
						break;
					}
				FinishPitch(now, winner, scoringTeam);
				return;
			}
		}

		ResetPitchBall();
		s_pitch.resetAt = now + kPitchResetMs;
		s_pitch.lastTouch = -1;
		return;
	}

	RegisterPitchCoronas();
}

// --- King of the Hill --------------------------------------------------------

void
TryStartKoth(uint32 now)
{
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	if(now < s_koth.rearmAt)
		return;

	const CVector centre = CrewCentre();
	int present = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(ped->DyingOrDead() || ped->m_nPedState == PED_ARRESTED)
			return;
		// The hill is on foot, in a car or in a Rhino.  A boat or an aircraft
		// is a different game; leave it for the water and the air modes.
		if(ped->bInVehicle && (ped->m_pMyVehicle == nil || !ped->m_pMyVehicle->IsCar()))
			return;
		if((PlayerCoors(i) - centre).Magnitude2D() > kKothGatherRange)
			return;
		present++;
	}
	if(present < 2)
		return;

	s_koth.active = true;
	s_koth.counting = true;
	s_koth.finished = false;
	s_koth.countdownAt = now + kKothCountdownMs;
	s_koth.endAt = s_koth.countdownAt + kKothRoundMs;
	s_koth.finishAt = 0;
	s_koth.rearmAt = 0;
	s_koth.groupSince = 0;
	s_koth.groupAll = false;
	s_koth.groupSpent = true;
	s_koth.centre = centre;
	s_koth.lastScoreAt = s_koth.countdownAt;
	s_koth.aloneSince = 0;
	s_koth.holder = -1;
	s_koth.inside = 0;
	s_koth.winner = -1;
	for(int i = 0; i < NUMPLAYERS; i++){
		s_koth.scoreMs[i] = 0;
		s_koth.downSince[i] = 0;
	}

	for(int i = 0; i < kKothRingMarks; i++){
		const float angle = 2.0f*PI*(float)i/(float)kKothRingMarks;
		CVector mark = centre + CVector(Cos(angle), Sin(angle), 0.0f)*kKothRadius;
		bool found = false;
		const float ground = CWorld::FindGroundZFor3DCoord(mark.x, mark.y, mark.z + 2.0f, &found);
		mark.z = (found ? ground : centre.z) + 1.0f;
		s_koth.ring[i] = mark;
	}

	COOP_LOG("WII coop: king of the hill on\n");
	CCoop::Notify("WII_CSK");
	ShowExplainer(EXPLAIN_KOTH, now);
}

void
RegisterKothCoronas(void)
{
	// Gold while the hill is open, the holder's colour while one player has
	// it, red while it is contested: the ring itself says who is scoring.
	CRGBA colour(255, 215, 80, 255);
	if(s_koth.inside >= 2)
		colour = CRGBA(255, 90, 80, 255);
	else if(s_koth.holder >= 0)
		colour = kCoopColours[s_koth.holder];
	for(int i = 0; i < kKothRingMarks; i++)
		CCoronas::RegisterCorona((uintptr)&s_koth + i, colour.r, colour.g, colour.b, 255,
			s_koth.ring[i], 2.5f, 250.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
			CCoronas::FLARE_NONE, CCoronas::REFLECTION_OFF,
			CCoronas::LOSCHECK_OFF, CCoronas::STREAK_OFF, 0.0f);
}

void
FinishKoth(uint32 now)
{
	int best = -1;
	uint32 bestScore = 0;
	bool tie = false;
	for(int i = 0; i < NUMPLAYERS; i++){
		// A player in the middle of the hill's own revive still has the time
		// they held; the scorer is not "not in the world" at the bell.
		if(s_koth.scoreMs[i] > bestScore){
			bestScore = s_koth.scoreMs[i];
			best = i;
			tie = false;
		}else if(s_koth.scoreMs[i] == bestScore && s_koth.scoreMs[i] > 0)
			tie = true;
	}
	if(!tie && best >= 0){
		s_koth.winner = best;
		CWorld::Players[CWorld::PlayerInFocus].m_nMoney += kKothPay;
		COOP_LOG("WII coop: king of the hill to player %d\n", best + 1);
	}else
		s_koth.winner = -1;
	s_koth.holder = -1;
	s_koth.inside = 0;
	s_koth.finished = true;
	s_koth.finishAt = now + kKothFinishMs;
}

// A burning, bombed or still-airborne vehicle is not a revive spot.
bool
KothVehicleSafe(CVehicle *vehicle)
{
	if(vehicle == nil || vehicle->GetStatus() == STATUS_WRECKED)
		return false;
	if(vehicle->m_pCarFire != nil || vehicle->m_fHealth < 250.0f || vehicle->m_nBombTimer > 0)
		return false;
	bool found = false;
	const CVector pos = vehicle->GetPosition();
	const float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
	// Five metres: still clearly in the air, not just a kerb or a jump.
	return found && pos.z - ground <= 5.0f;
}

// A spot beside a car the lead died in: the car is left abandoned, so putting
// the player back on its centre would put them inside the roof or the tank.
CVector
KothLeadSpot(const CVector &base, const CVector &right, const CVector &forward)
{
	const CVector offsets[4] = {
		right*4.0f, right*-4.0f, forward*4.0f, forward*-4.0f
	};
	for(int i = 0; i < 4; i++){
		CVector pos = base + offsets[i];
		bool found = false;
		const float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
		if(found){
			pos.z = ground;
			return pos;
		}
	}
	return base;
}

// Player 1 comes back at the body, not at a hospital.  The engine's own
// resurrection routine is used with its world-clearing step off: clearing a
// four-thousand-metre area here would delete the other players' cars and the
// hill around them.  The vehicle he died in is let go first, as the normal
// death path does, so it does not keep a dead driver.
void
ReviveKothLead(void)
{
	CPlayerPed *lead = CWorld::Players[0].m_pPed;
	if(lead == nil)
		return;

	CVector at = lead->GetPosition();
	const float angle = lead->m_fRotationCur;
	if(lead->bInVehicle && lead->m_pMyVehicle != nil){
		CVehicle *vehicle = lead->m_pMyVehicle;
		if(KothVehicleSafe(vehicle)){
			at = KothLeadSpot(vehicle->GetPosition(), vehicle->GetRight(), vehicle->GetForward());
		}else{
			// Do not come back under a burning or falling car: the hill's
			// centre is the safe ground the round is about.
			at = s_koth.centre;
			bool found = false;
			const float ground = CWorld::FindGroundZFor3DCoord(at.x, at.y, at.z + 2.0f, &found);
			if(found)
				at.z = ground;
		}
		if(vehicle->pDriver == lead){
			vehicle->pDriver = nil;
			if(vehicle->GetStatus() != STATUS_WRECKED)
				vehicle->SetStatus(STATUS_ABANDONED);
		}else
			vehicle->RemovePassenger(lead);
	}

	CGameLogic::RestorePlayerStuffDuringResurrection(lead, at, angle, false);
	COOP_LOG("WII coop: king of the hill player 1 back\n");
}

void
EndKoth(uint32 now, const char *why)
{
	s_koth.active = false;
	s_koth.counting = false;
	s_koth.finished = false;
	s_koth.groupSince = 0;
	s_koth.groupAll = false;
	s_koth.aloneSince = 0;
	s_koth.holder = -1;
	s_koth.inside = 0;
	for(int i = 0; i < NUMPLAYERS; i++)
		s_koth.downSince[i] = 0;
	s_koth.rearmAt = now + kKothRearmMs;
	COOP_LOG("WII coop: king of the hill off (%s)\n", why);
}

void
UpdateKoth(uint32 now)
{
	// Every present player holds aim: the group gesture.  The nod in a car is
	// the exit-vehicle button, so it would empty the cars the hill is meant to
	// allow; aim means the same thing on foot, in a car and in a Rhino.  It is
	// also counted here so the one-player nod modes wait while a group is
	// clearly asking for the hill instead.
	int present = 0;
	int holding = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		present++;
		if(GetPadFromPlayer(ped)->NewState.RightShoulder1 != 0)
			holding++;
	}
	s_koth.groupHold = holding;
	s_koth.groupAll = present >= 2 && holding == present;

	if(!s_koth.active){
		if(holding == 0)
			s_koth.groupSpent = false;
		if(s_koth.groupSpent){
			s_koth.groupSince = 0;
			return;
		}
		if(present >= 2 && holding == present){
			if(s_koth.groupSince == 0)
				s_koth.groupSince = now;
			else if(now - s_koth.groupSince >= kKothHoldMs){
				s_koth.groupSince = 0;
				TryStartKoth(now);
			}
		}else
			s_koth.groupSince = 0;
		return;
	}

	if(!CCoop::PairActive()){
		EndKoth(now, "session");
		return;
	}
	RegisterKothCoronas();

	if(s_koth.finished){
		if(now >= s_koth.finishAt)
			EndKoth(now, "over");
		return;
	}

	// A downed or arrested partner is brought back at the body after a few
	// seconds, so the hill does not turn into a two-player game the moment
	// somebody loses a fight.  The ordinary co-op revive is still there for
	// the players who reach a body sooner; this is the mode's own floor.
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil){
			s_koth.downSince[i] = 0;
			continue;
		}
		if(i == 0){
			if(ped->DyingOrDead()){
				if(s_koth.downSince[0] == 0)
					s_koth.downSince[0] = now;
				else if(now - s_koth.downSince[0] >= kKothReviveMs){
					ReviveKothLead();
					s_koth.downSince[0] = 0;
				}
			}else
				s_koth.downSince[0] = 0;
			continue;
		}
		if(ped->DyingOrDead() || ped->m_nPedState == PED_ARRESTED){
			if(s_koth.downSince[i] == 0)
				s_koth.downSince[i] = now;
			else if(now - s_koth.downSince[i] >= kKothReviveMs){
				CCoop::ReviveDownedPlayer(i);
				s_koth.downSince[i] = 0;
			}
		}else
			s_koth.downSince[i] = 0;
	}

	// A revive removes the ped for a frame or two, and a player walking out of
	// the party removes theirs for good.  The gap is not the round ending; a
	// player who never comes back is.
	if(present < 2){
		if(s_koth.aloneSince == 0)
			s_koth.aloneSince = now;
		else if(now - s_koth.aloneSince >= kKothAloneGraceMs){
			EndKoth(now, "alone");
			return;
		}
	}else
		s_koth.aloneSince = 0;

	if(s_koth.counting){
		if(now < s_koth.countdownAt)
			return;
		s_koth.counting = false;
		s_koth.lastScoreAt = now;
	}

	// Who is on the hill?  A player is wherever the game says they are: their
	// car or Rhino while they are in one.  Two players in one car are two
	// occupants, so the hill is contested.
	int inside = 0;
	int occupant = -1;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil || ped->DyingOrDead() || ped->m_nPedState == PED_ARRESTED)
			continue;
		// The hill starts on foot, in a car or in a Rhino.  A boat or an
		// aircraft that wanders into the ring is not a contender.
		if(ped->bInVehicle && (ped->m_pMyVehicle == nil || !ped->m_pMyVehicle->IsCar()))
			continue;
		if((PlayerCoors(i) - s_koth.centre).Magnitude2D() <= kKothRadius){
			inside++;
			occupant = i;
		}
	}
	s_koth.inside = inside;

	if(inside == 1){
		s_koth.holder = occupant;
		if(s_koth.scoreMs[occupant] < kKothRoundMs){
			const uint32 room = kKothRoundMs - s_koth.scoreMs[occupant];
			const uint32 delta = now - s_koth.lastScoreAt;
			s_koth.scoreMs[occupant] += Min(delta, room);
		}
	}else
		s_koth.holder = -1;
	s_koth.lastScoreAt = now;

	if(now >= s_koth.endAt)
		FinishKoth(now);
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

// Put a racer at the wheel of their toy rather than asking them to open a door
// the RC Bandit does not have.  Its model has no door components, and the
// normal walk-in path is for cars that do; the toy race seats the racer the way
// the game's own remote-control car seats nobody -- directly.
void
SeatRcRacer(int player)
{
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	CVehicle *car = s_rc.car[player];
	if(ped == nil || car == nil || ped->bInVehicle || ped->DyingOrDead() ||
	   ped->m_nPedState == PED_ARRESTED)
		return;
	// WarpPedIntoCar only takes a seat when the objective says which one.
	ped->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, car);
	if(ped->m_objective != OBJECTIVE_ENTER_CAR_AS_DRIVER)
		return;
	ped->WarpPedIntoCar(car);
	ped->SetObjective(OBJECTIVE_NONE);
	if(car->pDriver == ped)
		COOP_LOG("WII coop: player %d seated in rc car\n", player + 1);
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
	// Not ClearSpaceForMissionEntity: it deletes what it finds under the new
	// entity, and two racers in a knot can drop their toys on the same square.
	// The second call then deleted the first toy while s_rc.car still held it.
	// A toy is small and the grid is a few metres of clear apron.
	car->SetPosition(pos);
	// A toy is small enough that a matrix left in the constructor's orientation
	// reads as sideways at this distance.  Set the heading through the entity's
	// orientation and flush the frame after it.
	car->SetOrientation(0.0f, 0.0f, forward.Heading());
	car->GetMatrix().UpdateRW();
	car->UpdateRwFrame();
	car->SetStatus(STATUS_ABANDONED);
	car->bIsLocked = true;
	car->AutoPilot.m_nCarMission = MISSION_NONE;
	car->AutoPilot.m_nTempAction = TEMPACT_NONE;
	car->bEngineOn = true;
	car->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	CWorld::Add(car);
	car->RegisterReference((CEntity**)&s_rc.car[player]);
	s_rc.car[player] = car;
	COOP_LOG("WII coop: rc car %d at %.0f %.0f %.0f\n",
	         player + 1, pos.x, pos.y, pos.z);
	return true;
}

void
RcSlot(int player, CVector &out)
{
	// Put the toy just in front of its racer rather than in a line at the centre:
	// at the airport the line put some cars behind the camera, and a car on the
	// apron markings looked like it had spawned sideways.  The player is already
	// on the grid square, so this is both visible and immediately reachable.
	CPlayerPed *ped = CCoop::GetPlayerPed(player);
	if(ped != nil){
		CVector forward = ped->GetForward();
		forward.z = 0.0f;
		if(forward.Magnitude() < 0.1f)
			forward = s_rc.forward;
		forward.Normalise();
		out = ped->GetPosition() + forward*2.5f;
		return;
	}
	const CVector right(s_rc.forward.y, -s_rc.forward.x, 0.0f);
	out = s_rc.centre + right*((player - 1.5f) * 5.0f);
}

// A wrecked or lost toy is replaced beside the driver, once they are on their
// feet: the engine does the leaving itself, so there is no half-in, half-out
// ped to untangle, and the laps already driven stay.  The replacement seats
// them directly, for the same reason the grid does.
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
	SeatRcRacer(player);
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
	if(PartyWanted())
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
			SeatRcRacer(i);
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
			// and then they are seated in it.  Doing this every frame released
			// the last toy and raised a new one, dozens a second, until the
			// racer sat in one.  One on its way into a toy is not asking for
			// another.  The body is drawn again while they are out of it.
			ped->bRenderPedInCar = true;
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
	for(int i = 0; i < kDanceRingMarks; i++){
		const int player = i/(kDanceRingMarks/4);
		const CRGBA &colour = kCoopColours[player];
		CCoronas::RegisterCorona((uintptr)&s_dance + i,
			colour.r, colour.g, colour.b, 255,
			s_dance.ring[i], 2.5f, 150.0f, gpCoronaTexture[CCoronas::TYPE_STAR],
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
	for(int i = 0; i < kDanceRingMarks; i++){
		const float angle = 2.0f*PI*(float)i/(float)kDanceRingMarks;
		CVector mark = centre + CVector(Cos(angle), Sin(angle), 0.0f)*kDanceFloor;
		bool found = false;
		const float ground = CWorld::FindGroundZFor3DCoord(mark.x, mark.y, mark.z + 2.0f, &found);
		mark.z = (found ? ground : centre.z) + 1.0f;
		s_dance.ring[i] = mark;
	}
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
	if(PartyWanted())
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
	RegisterGroundRing((uintptr)&s_cnr + 8, s_cnr.getawayRing, kCnRRingMarks,
		CRGBA(120, 255, 130, 255), 2.2f);
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
	s_cnr.graceUntil = 0;
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

	for(int i = 0; i < kCnRRingMarks; i++){
		const float angle = 2.0f*PI*(float)i/(float)kCnRRingMarks;
		CVector mark = s_cnr.getaway + CVector(Cos(angle), Sin(angle), 0.0f)*kCnRDeliverRange;
		bool ringFound = false;
		const float ringGround = CWorld::FindGroundZFor3DCoord(mark.x, mark.y, mark.z + 2.0f, &ringFound);
		mark.z = (ringFound ? ringGround : s_cnr.getaway.z) + 1.0f;
		s_cnr.getawayRing[i] = mark;
	}

	s_cnr.getawayBlip = CRadar::SetCoordBlip(BLIP_COORD, s_cnr.getaway, RADAR_TRACE_GREEN, BLIP_DISPLAY_BOTH);
	CRadar::ChangeBlipColour(s_cnr.getawayBlip, RADAR_TRACE_GREEN);
	ShowLooseCase();
	COOP_LOG("WII coop: cops & robbers on\n");
	CCoop::Notify("WII_CSN");
	ShowExplainer(EXPLAIN_CNR, now);
}

void
TryStartCnR(uint32 now, bool held)
{
	// Every present player holding aim is the hill's.  See UpdateKoth.
	if(s_koth.groupAll)
		return;
	if(!CoopMinigames || !CCoop::PairActive())
		return;
	if(ModeActive())
		return;
	if(now < s_cnr.rearmAt)
		return;
	if(PartyWanted())
		return;
	if(!held)
		return;

	// The party on foot in a knot; the nod (Triangle) held for kCnRHoldMs starts it.
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
	// The nod is held, not tapped, to start a match: it is the enter-vehicle
	// button on foot, a prompt in the dance-off, and a tap as likely as not
	// meant for something else.  It is tracked every frame, press by press.  A
	// press that begins with a vehicle in reach is somebody getting in, and one
	// that begins during a match, another mode or the cool-down is somebody's
	// for another reason; neither starts anything, however long it is held, and
	// nor does one that has already been tried -- a button held down asks
	// once, not every frame.  Each is spent until it is let go.
	bool held = false;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil || GetPadFromPlayer(ped)->NewState.Triangle == 0){
			s_cnr.nodSince[i] = 0;
			s_cnr.nodSpent[i] = false;
			continue;
		}
		if(s_cnr.nodSpent[i])
			continue;
		if(s_cnr.nodSince[i] == 0){
			if(s_cnr.active || ModeActive() || now < s_cnr.rearmAt || VehicleInReach(ped))
				s_cnr.nodSpent[i] = true;
			else
				s_cnr.nodSince[i] = now;
		}else if(now - s_cnr.nodSince[i] >= kCnRHoldMs){
			held = true;
			s_cnr.nodSince[i] = 0;
			s_cnr.nodSpent[i] = true;
		}
	}

	if(!s_cnr.active){
		TryStartCnR(now, held);
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
			s_cnr.graceUntil = now + kCnRGraceMs;
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

		// Busted: a cop holds them for a moment.  Not in the first kCnRGraceMs
		// after the case was picked up -- nobody is pinning anyone then, so the
		// hold below starts when the grace is over and not before.
		int pin = -1;
		float pinDist = kCnRBustRange;
		for(int i = 0; i < NUMPLAYERS && now >= s_cnr.graceUntil; i++){
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
// ASCII on purpose -- a number, P2, GO, or one short status line.  AsciiToUnicode
// does not look at the size of what it writes into, so the buffer is generous
// and callers must still keep their line under it.
void
PrintCentred(float x, float y, const char *ascii, const CRGBA &colour, float scaleX, float scaleY)
{
	wchar text[128];
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
		"RACE FOR THE CARGO. WHOEVER GRABS IT RUNS IT TO THE RING; THE OTHER COVERS. 3 DROPS. P1 LEAVES BOAT TO QUIT",
		"RAMMING ONLY, NO SHOOTING. LAST CAR IN THE RING WINS THE ROUND. FIRST TO 2",
		"BOTH PLAYERS RACE THREE LAPS. FOLLOW YOUR COLOR OF GATE. NOT WHILE WANTED",
		"PRESS THE BUTTON ON YOUR CHIP BEFORE THE NEXT BEAT. BEST SCORE WINS. NOT WHILE WANTED",
		"ONE PLAYER HOLD NOD. GRAB THE CASE, RUN IT TO THE GREEN MARKER. COPS STAY CLOSE TO BUST. FIRST TO 3",
		"EACH IN A CAR; ONE HOLDS BRAKE. PUSH THE CADDY INTO A GOAL. 3 GOALS WINS. NOT WHILE WANTED",
		"ALL HOLD AIM. ONE IN THE RING SCORES. DAMAGE ON. 90 SECONDS"
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
	case EXPLAIN_CNR: live = s_cnr.active; break;
	case EXPLAIN_PITCH: live = s_pitch.active; break;
	case EXPLAIN_KOTH: live = s_koth.active; break;
	default: live = false; break;
	}
	if(!live)
		return;

	wchar text[128];
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
	char line[64];
	const CRGBA white(255, 255, 255, 255);

	// The cop shift: the streak, the case's clock, and what the last case came to.
	DrawShiftMarks(now);

	// The smuggling run: which drop, when the boat is unloading, and how many
	// boats are keeping up -- each of them pays at the drop.
	if(s_run.active){
		const int crew = 1 + (s_run.boat != nil ? CountEscorts(s_run.boat) : 0);
		CVehicle *carrierBoat = nil;
		if(s_run.carrier >= 0 && s_run.boat != nil)
			carrierBoat = BoatOfPlayer(s_run.carrier, s_run.boat);
		if(s_run.unloadSince != 0)
			sprintf(line, "UNLOADING %d/%d  CREW %d", s_run.leg + 1, kDrops, crew);
		else if(carrierBoat != nil)
			sprintf(line, "DROP %d/%d  P%d HAS CARGO  CREW %d",
				s_run.leg + 1, kDrops, s_run.carrier + 1, crew);
		else
			sprintf(line, "DROP %d/%d  RACE FOR CARGO  CREW %d", s_run.leg + 1, kDrops, crew);
		DrawStatus(line, s_run.unloadSince != 0 ? CRGBA(255, 200, 90, 255) : white);

		// The carrier has the drop to steer for and the other boat has to stay
		// with it.  Before the pickup the same line gives both boats their
		// distance to the crate, because the race for it is the job.
		if(s_run.boat != nil){
			char sub[96];
			bool away = false;
			if(carrierBoat != nil){
				const float dropDist = (carrierBoat->GetPosition() - s_run.drop).Magnitude2D();
				sprintf(sub, "DROP IN %dm", (int)dropDist);
				for(int i = 0; i < NUMPLAYERS; i++){
					if(i == s_run.carrier)
						continue;
					CVehicle *escort = BoatOfPlayer(i, s_run.boat);
					if(escort == nil)
						continue;
					const float dist = (escort->GetPosition() - carrierBoat->GetPosition()).Magnitude2D();
					char part[24];
					sprintf(part, "  P%d COVER %dm", i + 1, (int)dist);
					strcat(sub, part);
					away = away || dist > kEscortRange;
				}
			}else{
				sprintf(sub, "CARGO");
				for(int i = 0; i < NUMPLAYERS; i++){
					CVehicle *boat = BoatOfPlayer(i, s_run.boat);
					if(boat == nil)
						continue;
					const float dist = (boat->GetPosition() - s_run.cargoPos[s_run.leg]).Magnitude2D();
					char part[24];
					sprintf(part, "  P%d %dm", i + 1, (int)dist);
					strcat(sub, part);
				}
			}
			PrintCentred(SCREEN_WIDTH*0.5f, SCREEN_SCALE_Y(58.0f), sub,
				away ? CRGBA(255, 200, 90, 255) : white, 0.45f, 0.7f);
		}
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

	// Caddy football: the score, the lights, and the last goal.  The bar is a
	// cell a goal: in the three-goal game it is the player's own, in the team
	// games both members of a team carry the team's.
	if(s_pitch.active){
		if(s_pitch.finished){
			if(s_pitch.players == 3){
				DrawWinner(s_pitch.winner);
			}else{
				sprintf(line, "TEAM %d WINS", s_pitch.winnerTeam + 1);
				DrawStatus(line, s_pitch.winner >= 0 ? kCoopColours[s_pitch.winner] : white);
			}
		}else if(s_pitch.counting){
			DrawStatus("CADDY FOOTBALL", white);
			DrawCountdown(now, s_pitch.countdownAt);
		}else{
			if(s_pitch.players == 3){
				line[0] = '\0';
				for(int i = 0; i < NUMPLAYERS; i++){
					if(CCoop::GetPlayerPed(i) == nil)
						continue;
					char part[16];
					sprintf(part, "P%d %d  ", i + 1, s_pitch.score[i]);
					strcat(line, part);
				}
			}else
				sprintf(line, "TEAM 1 %d  TEAM 2 %d", s_pitch.teamScore[0], s_pitch.teamScore[1]);
			DrawStatus(line, white);

			if(s_pitch.lastGoalAt != 0 && now - s_pitch.lastGoalAt < kPitchGoalNoteMs){
				if(s_pitch.lastGoalBy >= 0){
					if(s_pitch.lastGoalOwn)
						sprintf(line, "OWN GOAL P%d", s_pitch.lastGoalBy + 1);
					else
						sprintf(line, "GOAL P%d", s_pitch.lastGoalBy + 1);
					PrintCentred(SCREEN_WIDTH*0.5f, SCREEN_SCALE_Y(70.0f), line,
						s_pitch.lastGoalOwn ? CRGBA(255, 200, 90, 255) : kCoopColours[s_pitch.lastGoalBy],
						1.0f, 1.5f);
				}else
					PrintCentred(SCREEN_WIDTH*0.5f, SCREEN_SCALE_Y(70.0f), "GOAL", white, 1.0f, 1.5f);
			}
		}
		for(int i = 0; i < NUMPLAYERS; i++){
			if(CCoop::GetPlayerPed(i) == nil)
				continue;
			const int score = s_pitch.players == 3 ? s_pitch.score[i] : s_pitch.teamScore[s_pitch.team[i]];
			DrawLaneBar(i, (float)score/(float)kPitchGoalsToWin, kPitchGoalsToWin);
		}
	}

	// King of the Hill: the round clock and who is on the hill, then a bar per
	// player for the time they have held it alone.
	if(s_koth.active){
		if(s_koth.finished){
			DrawWinner(s_koth.winner);
		}else if(s_koth.counting){
			DrawStatus("KING OF THE HILL", white);
			DrawCountdown(now, s_koth.countdownAt);
		}else{
			char koth[48];
			const uint32 left = s_koth.endAt > now ? (s_koth.endAt - now + 999)/1000 : 0;
			if(s_koth.downSince[0] != 0 && now - s_koth.downSince[0] < kKothReviveMs){
				const uint32 wait = (kKothReviveMs - (now - s_koth.downSince[0]) + 999)/1000;
				sprintf(koth, "KING OF THE HILL  %us  P1 DOWN %us", left, wait);
				DrawStatus(koth, CRGBA(255, 200, 90, 255));
			}else if(s_koth.inside >= 2)
				sprintf(koth, "KING OF THE HILL  %us  CONTESTED", left);
			else if(s_koth.holder >= 0)
				sprintf(koth, "KING OF THE HILL  %us  P%d HOLDS", left, s_koth.holder + 1);
			else
				sprintf(koth, "KING OF THE HILL  %us  GET IN", left);
			if(s_koth.downSince[0] == 0 || now - s_koth.downSince[0] >= kKothReviveMs)
				DrawStatus(koth, s_koth.holder >= 0 ? kCoopColours[s_koth.holder] :
				           s_koth.inside >= 2 ? CRGBA(255, 120, 120, 255) : white);
		}
		for(int i = 0; i < NUMPLAYERS; i++){
			if(CCoop::GetPlayerPed(i) == nil)
				continue;
			DrawLaneBar(i, (float)s_koth.scoreMs[i]/(float)kKothRoundMs, kKothBarCells);
			float cx, cy;
			LaneSlot(i, cx, cy);
			char secs[16];
			sprintf(secs, "%us", s_koth.scoreMs[i]/1000);
			PrintCentred(cx, cy + SCREEN_SCALE_Y(16.0f), secs, white, 0.45f, 0.7f);
		}
	}

	// The race: the lights, then a bar per racer for how far round they are,
	// a cell to a lap, and which lap they are on.
	if(s_rc.active){
		if(s_rc.finished)
			DrawWinner(s_rc.winner);
		else if(s_rc.grid)
			DrawStatus("RC RACE  GET READY", white);
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
			sprintf(line, "P%d HAS THE CASE%s", s_cnr.carrier + 1, now < s_cnr.graceUntil ? "  SAFE" : "");
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

// --- the menu's starts -------------------------------------------------------

int
MenuRequiredModel(int mode)
{
	switch(mode){
	case MENU_SHIFT: return MI_POLICE;
	case MENU_RUN: return MI_SQUALO;
	case MENU_RC: return MI_RCBANDIT;
	case MENU_PITCH: return MI_CADDY;
	default: return -1;
	}
}

CVehicle *
MenuSpawnPolice(const CVector &at, float heading)
{
	if(!CStreaming::HasModelLoaded(MI_POLICE))
		return nil;
	CAutomobile *car = new CAutomobile(MI_POLICE, MISSION_VEHICLE);
	if(car == nil)
		return nil;

	CVector pos = MenuGround(at, 0.0f);
	pos.z += car->GetDistanceFromCentreOfMassToBaseOfModel();
	car->SetPosition(pos);
	car->SetOrientation(0.0f, 0.0f, heading);
	CTheScripts::ClearSpaceForMissionEntity(pos, car);
	car->GetMatrix().UpdateRW();
	car->UpdateRwFrame();
	car->SetStatus(STATUS_ABANDONED);
	car->bIsLocked = false;
	car->AutoPilot.m_nCarMission = MISSION_NONE;
	car->AutoPilot.m_nTempAction = TEMPACT_NONE;
	car->bEngineOn = true;
	car->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	if(!car->bIsLawEnforcer)
		car->ChangeLawEnforcerState(true);
	car->m_bSirenOrAlarm = true;
	CWorld::Add(car);
	if(!MenuStoreLoaner(car)){
		CTheScripts::CleanUpThisVehicle(car);
		return nil;
	}
	return car;
}

CVehicle *
MenuSpawnBoat(const CVector &at, float heading)
{
	if(!CStreaming::HasModelLoaded(MI_SQUALO))
		return nil;
	CBoat *boat = new CBoat(MI_SQUALO, MISSION_VEHICLE);
	if(boat == nil)
		return nil;

	CVector pos = at;
	pos.z += boat->GetDistanceFromCentreOfMassToBaseOfModel();
	boat->SetPosition(pos);
	boat->SetOrientation(0.0f, 0.0f, heading);
	boat->GetMatrix().UpdateRW();
	boat->UpdateRwFrame();
	boat->SetStatus(STATUS_ABANDONED);
	boat->bIsLocked = true;
	boat->AutoPilot.m_nCarMission = MISSION_NONE;
	boat->AutoPilot.m_nTempAction = TEMPACT_NONE;
	boat->bEngineOn = true;
	boat->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	CWorld::Add(boat);
	if(!MenuStoreLoaner(boat)){
		CTheScripts::CleanUpThisVehicle(boat);
		return nil;
	}
	return boat;
}

bool
StartMenuShift(uint32 now)
{
	if(PartyWanted())
		return false;

	CVector spot;
	if(!FindCarNodeNear(kShiftMenuBase, 300.0f, spot))
		spot = kShiftMenuBase;
	MenuLoadScene(spot);
	if(!MenuEnsureModel(MI_POLICE))
		return false;
	spot = MenuGround(spot, 0.0f);

	CPlayerPed *lead = CCoop::GetPlayerPed(0);
	if(lead == nil)
		return false;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil && ped->bInVehicle)
			MenuLeaveVehicle(ped);
	}

	CVehicle *car = MenuSpawnPolice(spot, 0.0f);
	if(car == nil || !SeatMenuDriver(lead, car))
		return false;

	for(int i = 1; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(MenuSeatPassenger(ped, car))
			continue;
		// A full car is not a reason to leave somebody behind: a second
		// cruiser is still a crewed law car, and the shift's own check only
		// needs the largest one.  Put it beside the first on real ground,
		// rather than on a kerb that happens to be eight metres east.
		const CVector beside = MenuGround(CVector(spot.x + 8.0f, spot.y, spot.z), 0.0f);
		CVehicle *second = MenuSpawnPolice(beside, 0.0f);
		if(second == nil || !SeatMenuDriver(ped, second))
			return false;
		car = second;
	}

	CCoop::ClearTeleportWatch();
	TryStartShift(now);
	return s_shift.active;
}

bool
StartMenuRun(uint32 now)
{
	CVector water;
	if(!FindWaterNodeNear(kRunMenuBase, 500.0f, water))
		return false;
	MenuLoadScene(water);
	if(!MenuEnsureModel(MI_SQUALO))
		return false;

	CPlayerPed *lead = CCoop::GetPlayerPed(0);
	if(lead == nil)
		return false;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped != nil && ped->bInVehicle)
			MenuLeaveVehicle(ped);
	}

	CVehicle *boat = MenuSpawnBoat(water, 0.0f);
	if(boat == nil || !SeatMenuDriver(lead, boat))
		return false;
	if(!StartRunAt(now, boat))
		return false;

	for(int i = 1; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		CVehicle *escort = CCoopModes::EscortBoat(i, boat);
		if(escort == nil || !SeatMenuDriver(ped, escort)){
			EndRun(now, "menu start");
			return false;
		}
	}

	CCoop::ClearTeleportWatch();
	return true;
}

bool
StartMenuFoot(int mode, const MenuVenue &venue, uint32 now)
{
	if(mode != MENU_KOTH && PartyWanted())
		return false;

	// The beach points were picked from the map's empty ground, but the sea is
	// close enough that a wrong one puts the party in water.  Load the venue
	// first, then keep it only if there is real ground above the waterline; the
	// airport apron is the safe fallback for the foot modes.
	MenuVenue actual = venue;
	MenuLoadScene(actual.pos);
	if(!MenuVenueOnLand(actual, nil)){
		COOP_LOG("WII coop: menu venue is water, using airport\n");
		actual = kAirportMenu;
		MenuLoadScene(actual.pos);
	}
	if(!MenuPrepareFoot(actual))
		return false;
	// The airport scene switch can drop a requested model that has not been
	// committed yet.  The toy race cannot start without its cars, so make sure
	// the model is here before the mode reports itself active.
	if(mode == MENU_RC && !MenuEnsureModel(MI_RCBANDIT))
		return false;
	const CVector centre = CrewCentre();
	const CVector forward(-Sin(actual.heading), Cos(actual.heading), 0.0f);

	switch(mode){
	case MENU_RC:
		if(PartyWanted())
			return false;
		s_rc.rearmAt = 0;
		StartRc(now, centre, forward);
		return s_rc.active;
	case MENU_DANCE:
		if(PartyWanted())
			return false;
		s_dance.rearmAt = 0;
		StartDance(now, centre);
		return s_dance.active;
	case MENU_CNR:
		if(PartyWanted())
			return false;
		s_cnr.rearmAt = 0;
		StartCnR(now, centre, forward);
		return s_cnr.active;
	case MENU_KOTH:
		s_koth.rearmAt = 0;
		TryStartKoth(now);
		return s_koth.active;
	default:
		return false;
	}
}

bool
StartMenuCars(int mode, uint32 now)
{
	const bool caddy = mode == MENU_PITCH;
	if(caddy && PartyWanted())
		return false;
	if(!MenuPrepareCars(kAirportMenu, caddy))
		return false;

	if(caddy){
		// The ball is a Caddy model of its own, and a pitch with no ball is not
		// a match.  The airport scene switch can drop a request, so ensure it
		// after the scene is loaded as well as before the mode starts.
		if(!MenuEnsureModel(MI_CADDY))
			return false;
		// The gesture check gives King of the Hill first refusal on a group
		// aim-hold; a menu selection is already that decision.
		const bool group = s_koth.groupAll;
		s_koth.groupAll = false;
		s_pitch.rearmAt = 0;
		TryStartPitch(now, true);
		s_koth.groupAll = s_pitch.active ? false : group;
		return s_pitch.active;
	}

	s_derby.rearmAt = 0;
	StartDerby(now, CrewCentre());
	return s_derby.active;
}

bool
StartMenuMode(int mode, uint32 now)
{
	ReleaseMenuLoaners();

	int present = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(ped->DyingOrDead() || ped->m_nPedState == PED_ARRESTED)
			return false;
		present++;
	}
	if(present < 2)
		return false;

	switch(mode){
	case MENU_SHIFT: return StartMenuShift(now);
	case MENU_RUN: return StartMenuRun(now);
	case MENU_DERBY: return StartMenuCars(MENU_DERBY, now);
	case MENU_RC: return StartMenuFoot(MENU_RC, kAirportMenu, now);
	case MENU_DANCE: return StartMenuFoot(MENU_DANCE, kDanceMenu, now);
	case MENU_CNR: return StartMenuFoot(MENU_CNR, kCnRMenu, now);
	case MENU_PITCH: return StartMenuCars(MENU_PITCH, now);
	case MENU_KOTH: return StartMenuFoot(MENU_KOTH, kKothMenu, now);
	default: return false;
	}
}

// Kept out of CCoopModes::Update: its teleport/start paths are much too large
// to inline, and GCC 12's CFG cleanup chokes on the merged function.
__attribute__((noinline)) void
UpdateMenuPending(uint32 now)
{
	if(s_menuPending.mode < 0)
		return;
	// The page is still open: the player may be changing their mind or
	// backing out, and teleporting the world behind it is not a menu's job.
	// The clock is held too, so a long look at the page does not eat the
	// model-loading budget.
	if(FrontEndMenuManager.GetIsMenuActive()){
		s_menuPending.requestedAt = now;
		return;
	}
	if(!CoopMinigames || !CCoop::PairActive() || ModeActive()){
		s_menuPending.mode = -1;
		return;
	}

	const int model = MenuRequiredModel(s_menuPending.mode);
	if(model >= 0 && !CStreaming::HasModelLoaded(model)){
		CStreaming::RequestModel(model, 0);
		if(now - s_menuPending.requestedAt > 60000){
			COOP_LOG("WII coop: menu start model %d never arrived\n", model);
			s_menuPending.mode = -1;
		}
		return;
	}

	const int mode = s_menuPending.mode;
	if(StartMenuMode(mode, now)){
		s_menuPending.mode = -1;
		return;
	}
	ReleaseMenuLoaners();
	COOP_LOG("WII coop: menu start %d failed\n", mode);
	s_menuPending.mode = -1;
}

} // namespace

void
CCoopModes::Init(void)
{
	s_menuPending.mode = -1;
	s_menuPending.requestedAt = 0;
	if(s_shift.suspectCar != nil)
		s_shift.suspectCar->CleanUpOldReference((CEntity**)&s_shift.suspectCar);
	if(s_shift.suspectPed != nil)
		s_shift.suspectPed->CleanUpOldReference((CEntity**)&s_shift.suspectPed);
	memset(&s_shift, 0, sizeof(s_shift));

	if(s_run.boat != nil)
		s_run.boat->CleanUpOldReference((CEntity**)&s_run.boat);
	for(int i = 0; i < NUMPLAYERS; i++){
		if(s_run.escort[i] != nil)
			s_run.escort[i]->CleanUpOldReference((CEntity**)&s_run.escort[i]);
	}
	for(int i = 0; i < 2; i++){
		if(s_run.pursuer[i] != nil)
			s_run.pursuer[i]->CleanUpOldReference((CEntity**)&s_run.pursuer[i]);
		if(s_run.pursuerDriver[i] != nil)
			s_run.pursuerDriver[i]->CleanUpOldReference((CEntity**)&s_run.pursuerDriver[i]);
	}
	memset(&s_run, 0, sizeof(s_run));
	// A cleared blip has to read as "none": id 0 is a real blip.
	s_run.blip = -1;
	s_run.cargoBlip = -1;
	s_run.objectiveBlip = -1;
	s_run.objectiveHandle = -1;
	s_run.cargoPlanned = false;
	s_run.firstHeat = false;
	s_run.carrier = -1;
	s_run.unloadBoat = -1;

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

	if(s_pitch.ball != nil)
		s_pitch.ball->CleanUpOldReference((CEntity**)&s_pitch.ball);
	memset(&s_pitch, 0, sizeof(s_pitch));
	s_pitch.lastTouch = -1;
	s_pitch.lastGoalBy = -1;
	s_pitch.winner = -1;
	s_pitch.winnerTeam = -1;

	memset(&s_koth, 0, sizeof(s_koth));
	s_koth.holder = -1;
	s_koth.winner = -1;

	memset(&s_explain, 0, sizeof(s_explain));
}

void
CCoopModes::Update(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();

	// A row on the minigame page arms a start; this is the first point after
	// the menu has closed that can stream the destination and move anybody.
	UpdateMenuPending(now);

	// The explainers are for the first time a mode arms in a session.  The
	// session is what every mode below asks CCoop::PairActive about, and it is
	// over for a mission, a cutscene, player 1 down, or the party gone: the
	// next arming is a new session's, and is told again.
	if(!CCoop::PairActive())
		memset(&s_explain, 0, sizeof(s_explain));

	// The modes are independent; all ride the same session state.  King of the
	// Hill goes first: its group nod must claim the press before the
	// one-player nod modes can read the same hold.
	UpdateKoth(now);
	UpdateRun(now);
	UpdatePitch(now);
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

CVehicle *
CCoopModes::EscortBoat(int player, CVehicle *lead)
{
	if(!s_run.active || lead == nil || lead != s_run.boat || player < 1 || player >= NUMPLAYERS)
		return nil;
	if(lead->GetStatus() == STATUS_WRECKED || !CStreaming::HasModelLoaded(lead->GetModelIndex()))
		return nil;

	// The one this partner was given before, if it is still afloat, empty, and
	// with the lead; a retry after a failed arrival must not make a boat each
	// time.
	CVehicle *old = s_run.escort[player];
	if(old != nil){
		if(old->GetStatus() != STATUS_WRECKED && old->pDriver == nil &&
		   (old->GetPosition() - lead->GetPosition()).Magnitude() < kEscortRange)
			return old;
		ReleaseEscort(player);
	}

	CVector spot;
	if(!FindEscortSpot(player, lead, spot))
		return nil;

	// The same model as the lead's, which is in memory because the lead is in
	// it.  A mission boat, locked and with nobody in it, made the way the
	// Predators are; the partner is put at its wheel by the caller.
	CBoat *boat = new CBoat(lead->GetModelIndex(), MISSION_VEHICLE);
	if(boat == nil)
		return nil;
	spot.z += boat->GetDistanceFromCentreOfMassToBaseOfModel();
	boat->SetPosition(spot);
	boat->SetOrientation(0.0f, 0.0f, lead->GetForward().Heading());
	boat->GetMatrix().UpdateRW();
	boat->UpdateRwFrame();
	boat->SetStatus(STATUS_ABANDONED);
	boat->bIsLocked = true;
	boat->AutoPilot.m_nCarMission = MISSION_NONE;
	boat->AutoPilot.m_nTempAction = TEMPACT_NONE;
	boat->bEngineOn = true;
	boat->m_nZoneLevel = CTheZones::GetLevelFromPosition(&spot);
	// Keeping pace with the lead from the first frame, as a regrouped boat does.
	boat->m_vecMoveSpeed = lead->GetMoveSpeed();
	CWorld::Add(boat);
	boat->RegisterReference((CEntity**)&s_run.escort[player]);
	s_run.escort[player] = boat;
	COOP_LOG("WII coop: escort boat for player %d\n", player + 1);
	return boat;
}

bool
CCoopModes::SteadyView(void)
{
	// The derby, where every car is spinning, the toy race, where every car is
	// a twitchy little thing that would whip the view around, Caddy football,
	// where the pitch should hold still while four cars fight over one golf
	// cart, and King of the Hill, which is one fixed ring.  See
	// CCam::Process_WiiCoop.
	return s_derby.active || s_rc.active || s_pitch.active || s_koth.active;
}

bool
CCoopModes::MenuStart(int mode)
{
	if(!CoopMinigames || !CCoop::PairActive() || ModeActive())
		return false;
	if(mode < MENU_SHIFT || mode > MENU_KOTH)
		return false;

	int present = 0;
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CCoop::GetPlayerPed(i);
		if(ped == nil)
			continue;
		if(ped->DyingOrDead() || ped->m_nPedState == PED_ARRESTED)
			return false;
		present++;
	}
	if(present < 2)
		return false;

	// Arm it and let Update do the work once the menu is out of the way.  The
	// scene load can block for a moment, and the menu is still drawing.
	s_menuPending.mode = mode;
	s_menuPending.requestedAt = CTimer::GetTimeInMilliseconds();
	const int model = MenuRequiredModel(mode);
	if(model >= 0)
		CStreaming::RequestModel(model, 0);
	COOP_LOG("WII coop: menu minigame %d armed\n", mode);
	return true;
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
CCoopModes::CameraHint(CVector &target)
{
	if(!s_cnr.active || s_cnr.finished)
		return false;
	if(s_cnr.carrier >= 0 && CCoop::GetPlayerPed(s_cnr.carrier) != nil)
		target = PlayerCoors(s_cnr.carrier);
	else
		target = s_cnr.casePos;
	return true;
}

bool
CCoopModes::KothLeadDown(void)
{
	if(!s_koth.active || s_koth.finished)
		return false;
	CPlayerPed *lead = CWorld::Players[0].m_pPed;
	return lead != nil && lead->DyingOrDead();
}

bool
CCoopModes::ForceFriendlyFire(void)
{
	// King of the Hill is a fight over a ring: the toggle is suspended for
	// the round, or a party with friendly fire off could stand in the ring
	// together and nobody could take it from anybody.  Once the bell has
	// gone, the round is over and the toggle is the party's again.
	return s_koth.active && !s_koth.finished;
}

int
CCoopModes::DownedBleedMs(int normal)
{
	// The hill brings a partner back at kKothReviveMs; the HUD's down bar has
	// to count the same clock, or it would still be showing seconds when they
	// are already on their feet.  After the bell the hill's revive is over,
	// so the ordinary bar is the honest one again.
	return s_koth.active && !s_koth.finished ? (int)kKothReviveMs : normal;
}

bool
CCoopModes::IsModeVehicle(const CVehicle *vehicle)
{
	if(vehicle == nil)
		return false;
	if(vehicle == s_shift.suspectCar)
		return true;
	for(int i = 0; i < NUMPLAYERS; i++){
		if(vehicle == s_derby.loaner[i] || vehicle == s_rc.car[i] || vehicle == s_run.escort[i])
			return true;
	}
	return vehicle == s_run.pursuer[0] || vehicle == s_run.pursuer[1] || vehicle == s_pitch.ball;
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
