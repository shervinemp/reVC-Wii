#include "common.h"

#include "Coop.h"
#include "AimAssist.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Darkel.h"
#include "Draw.h"
#include "General.h"
#include "Hud.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "Pad.h"
#include "PedIK.h"
#include "PedPlacement.h"
#include "PlayerPed.h"
#include "WiiPointerAim.h"
#include "Pools.h"
#include "Replay.h"
#include "Script.h"
#include "Sprite.h"
#include "Streaming.h"
#include "Text.h"
#include "Timer.h"
#include "Vehicle.h"
#include "Wanted.h"
#include "WaterLevel.h"
#include "WeaponInfo.h"
#include "World.h"
#include "Zones.h"
#ifdef NINTENDO_WII
#include "WiiTrace.h"
#endif

// Transitions only.  Co-op cannot be run on anything but a console with two
// people in front of it, so when it misbehaves the log is the whole of the
// evidence -- but a session that is working writes a handful of these lines
// and no more, which keeps it inside the "signal, not trace" budget the rest of
// the port's logging holds to.
#ifdef NINTENDO_WII
#define COOP_LOG(...) WiiTraceReport(__VA_ARGS__)
#else
#define COOP_LOG(...) debug(__VA_ARGS__)
#endif

bool CCoop::ms_bRunning;
int8 CCoop::ms_nFraming = 1;
bool CCoop::ms_bPartnerFocus = false;

// The co-op options that are not pad settings, set from the co-op page and
// saved in the INI under "Wii".  Friendly fire is on by default -- the game as
// it was, with no rule against players shooting each other -- and the pair
// shares one wanted level, which is what the pair was designed around.  With
// sharing off a partner's crimes are still the pair's problem, because the
// police only ever read player 1; see ShareWantedLevel.
int8_t CoopFriendlyFire = 1;
int8_t CoopSharedWanted = 1;

namespace
{

enum
{
	LEAD = 0,
	PARTNER = 1,
};

// --- when the partner comes and goes ---------------------------------------
// A second controller being connected is not a second player.  A remote is
// connected whenever it is switched on, and the one that started the game is
// still switched on while its owner plays on a GameCube pad: taking that for
// a partner would stand an idle Tommy beside everyone who plays that way.  So
// the partner arrives when somebody presses a button on it.
//
// It has to have been there this long before a button counts.  A Wiimote
// reports its Nunchuk a few frames after it reports itself, and until then it
// is not yet the controller its player is holding.
const uint32 kJoinAfterMs = 400;
// And gone this long before they leave.  Long, because a remote drops out for a
// second or two whenever its batteries sag or someone sits on it, and losing
// the partner to that would read as the game throwing them out.
const uint32 kDropAfterMs = 6000;
// How long a downed partner is left where they fell before coming back beside
// player 1.  Long enough for the other player to reach them -- which is what
// brings them back where they are instead -- and short enough not to sit out.
const uint32 kDownedBleedMs = 10000;
// How close another player has to be to a downed partner to bring them back,
// and how long they have to stay there.  The hold is what makes it something
// a player does rather than something that happens to whoever walks past.
const float kReviveRange = 2.0f;
const uint32 kReviveHoldMs = 1000;
// How long to wait before trying again when there was nowhere to put them.
const uint32 kRetryMs = 400;

// --- staying together --------------------------------------------------------
// There is one screen, and the world only exists around player 1 -- collision,
// the map and the traffic are all streamed in around FindPlayerCoors() -- so
// the two players are kept within reach of each other, in two ways.
//
// On foot it is a wall.  Past kTetherStart a player walking away from the
// other is slowed, and at kTetherMax they are stopped: neither can leave the
// other behind, and both have to mean to go somewhere to get there.  It holds
// each of them equally; see CCoop::LimitSeparation.
//
// The wall only works on someone walking.  A car driving off, a fall, the
// blast from an explosion all get past it, and for those there is kLeash: the
// partner is brought back to player 1 -- into the passenger seat, if player 1
// is the one in the car.
//
// kCoopMaxDistance in Cam.cpp is sized to keep both players on screen out to
// the wall, in the direction the camera sees least far.
const float kTetherStart = 18.0f;
const float kTetherMax = 24.0f;
const float kLeash = 28.0f;
// And the longer one a driver gets.  A car covers ground far faster than a walk,
// and a convoy is useless if the one behind is hauled forward every second -- at
// 28 m that is about a second at speed.  Still bounded, because the leash exists
// for the streaming and not for taste; this is the number to move if the ground
// under a driving partner ever looks thin, and it wants checking on hardware
// rather than trusting.
const float kLeashDriving = 50.0f;
// Where a car the leash has caught is put: this far back along player 1's
// heading, and never within this much of the car player 1 is driving.  The node
// nearest player 1 is the one their own car is standing on, and two cars in the
// same square metre spend the next few seconds pushing each other out -- which
// is what a regroup that fires every second looks like from the sofa.
const float kRegroupBack = 12.0f;
const float kRegroupClearance = 8.0f;
const float kRegroupSearch = 30.0f;
// Below player 1 by this much means they fell through something.
const float kFallLimit = 25.0f;
// Player 1 moving further than this in one frame was a teleport, not travel.
const float kTeleportStep = 12.0f;
// A vehicle slower than this (world units per game step, about 5 m/s) is
// near enough to stopped for someone to be put down beside it.
const float kSlowVehicle = 0.1f;
// How close the partner has to be to get into player 1's car by pressing the
// button, rather than being expected to walk to it first.
const float kBoardingRange = 12.0f;
// How close a car has to be for the partner to take it as its driver when
// there is no seat with player 1.  Matches the box CPlayerInfo::Process scans
// for player 1's own enter/exit button.
const float kStealRange = 10.0f;

// --- aiming ------------------------------------------------------------------
// A pointer that has not reported for this long is one nobody is aiming with.
const uint32 kPointerFreshMs = 1500;
// The right stick, for a player on a pad with no pointer: how far it has to be
// pushed to mean a direction (of 128), how long that direction is kept after it
// is let go, and how far away along it the reticle is put.
const float kStickAimThreshold = 48.0f;
const uint32 kStickHoldMs = 1500;
const float kStickReach = 9.0f;
// How far the ray under a reticle is followed: this far past the point where it
// comes down to the player's own level, and never further than the second.
// It is a world ray per player per frame, so it is kept as short as it can be.
const float kReticleRayPast = 8.0f;
const float kReticleRay = 150.0f;
// Closer to the player than this and "which way is the reticle" is noise.
const float kMinAimDistance = 0.75f;
// A shot goes to the reticle when the gun is pointing within this much of it,
// and straight out of the gun when it is not.  The body turns toward the
// reticle at its own pace, half a turn takes it several frames, and a player
// who is sprinting is not facing it at all; a round that left for the reticle
// regardless would leave sideways or backwards, through the shooter.
const float kAimArc = DEGTORAD(50.0f);

// The assist: a reticle that is on nothing still bends a shot toward a target
// that is nearly in line.  Same shape as CAimAssist's cone, and for the same
// reason -- a target should be as easy to catch far away as close up, so the
// cone follows how big the target looks rather than being a fixed angle.
const float kAssistRadius = 1.1f;		// metres
const float kAssistMinCone = DEGTORAD(2.5f);
const float kAssistMaxCone = DEGTORAD(8.0f);
const uint32 kAssistSightMs = 250;		// how often its line of sight is rechecked

struct CoopAim
{
	// What the player is aiming with, refreshed as it arrives.
	float pointerX, pointerY;
	uint32 pointerTime;
	bool pointerSeen;
	float stickHeading;
	uint32 stickTime;
	bool stickSeen;

	// What that comes to, worked out once a frame.
	bool active;		// there is something to aim with
	bool hasTarget;		// point is a thing, not just a direction
	bool engaged;		// and that thing is a live target, for the HUD
	bool facing;		// the body faces the reticle this frame
	bool drawReticle;
	CVector point;
	float heading;
	float rawHeading;	// the heading as aimed, before the assist bent it
	float reticleX, reticleY;

	int32 assistHandle;
	uint32 assistSightTime;
	bool assistVisible;
};
CoopAim s_aim[2];

// The partner's controller: whether it is there now, since when, and when it
// was last there.
bool s_padHere;
uint32 s_padSince;
uint32 s_padLast;
bool s_padEver;
// Whether somebody has pressed a button on it since it turned up, and whether
// they have been told that is what it takes.
bool s_joined;
bool s_joinTold;
// When the partner may next be put into the world.
uint32 s_spawnTime;
// When the regroup line was last written.  It is a signal, not a tick: a car
// being yanked every second must not write it every second too.
uint32 s_regroupLogTime;
// When the partner was first seen down -- dead, or out of health in a car with
// an order to die on the pavement -- or 0.  Arrested counts too: that is the
// one of the three nobody can be brought back from.
uint32 s_downTime;
// Since when another player has been close enough to revive them, or 0.  A
// revive needs the hold, so walking past a body does not do it.
uint32 s_reviveTime;
// Where a revived partner is put back: where they fell, not beside player 1.
// Consumed by the next SpawnPartner, and dropped if the session goes down
// first.
CVector s_reviveAt;
bool s_reviveValid;
// Where player 1 was last frame, to notice a teleport.
CVector s_leadPos;
bool s_leadPosValid;
// Whether the players have been told about the wall yet.
bool s_tetherTold;
// Whether the partner's next arrival is news.  Coming back from the dead or
// from the far end of the street is not; a controller being picked up is.
bool s_announceJoin;

// A line in the help box, for the few things that happen with nothing on screen
// to show for them: a second controller being noticed, the partner going
// because theirs went, the framing changing.  The words are in Text.cpp's
// fallback table, since the user's own GXT files are never rewritten.
void
Tell(const char *key)
{
	CHud::SetHelpMessage(TheText.Get(key), true);
}

// What a partner who is only being moved, not replaced, keeps.  No ammo: the
// weapons are, and the ammo is the shared pool.
struct Carry
{
	bool valid;
	float health;
	float armour;
	int8 slot;
	eWeaponType weapon[TOTAL_WEAPON_SLOTS];
};
Carry s_carry;
// The weapon slot the partner should end up holding once it arrives.
int8 s_wantSlot = -1;
uint32 s_wantSlotUntil;

// --- the partner's weapons ---------------------------------------------------
// Pickups, shops and scripts all hand weapons to FindPlayerPed(), and that is
// left exactly as it is.  The partner is given the same WEAPONS as player 1 --
// one pickup arms both -- but the two draw from ONE AMMO POOL, not a copy each.
//
// The pool is player 1's m_nAmmoTotal and the partner's is kept equal to it.
// The clip and the reload timer stay each player's own: they are what makes a
// reload take time, and sharing the clip too would let two magazines of rounds
// exist against one count.  When the partner fires, their clip drops as usual
// and the rounds they used are taken out of the pool at the next
// CCoop::Update (SyncSharedAmmo).  So a pickup arms both, a burst from one
// empties the other, and neither gets a second copy of the same rounds.
//
// This is why the partner's own total is never trusted: it is only a cache of
// the pool, re-synced every frame.  A magazine is clamped to what the pool has
// left, so it cannot hold rounds the other player has already spent.
//
// What has been seen of player 1's weapons is kept for as long as the game is,
// not for as long as the partner is.  A partner who is only being moved takes
// their own weapons with them (Carry); one who is new, or who died, starts
// from nothing seen and so from a copy of everything player 1 has.
eWeaponType s_seenType[TOTAL_WEAPON_SLOTS];
// Owed but not yet handed over, because its model is still streaming in.  Given
// with no ammo: the pool is the ammo.
eWeaponType s_owedType[TOTAL_WEAPON_SLOTS];
// The shared pool as of the last sync, per slot.  -1 means this slot is not
// shared yet (nothing there, or a weapon the partner has not been given).
int32 s_poolAmmo[TOTAL_WEAPON_SLOTS];

// --- the wanted level ---------------------------------------------------------
// One heat for the pair, in the direction that matters: a crime by either is
// the pair's crime, so when either player's level rises both go to the higher
// of the two -- and the stars on the HUD, player 1's, are the ones the pair is
// judged by.  Drops are left alone: each level decays on its own, and holding
// both at the higher of the two every frame would keep the pair wanted for
// good.
//
// The one cost is a statistic: SetWantedLevel is also what counts
// CStats::WantedStarsAttained and WantedStarsEvaded, so the pair's crimes and
// evasions count those twice.  Nothing reads them but the stats screen.
int32 s_leadWanted;
int32 s_partnerWanted;

void
ShareWantedLevel(CPlayerPed *lead, CPlayerPed *partner)
{
	if(lead->m_pWanted == nil || partner->m_pWanted == nil)
		return;
	const int32 leadLevel = lead->m_pWanted->GetWantedLevel();
	const int32 partnerLevel = partner->m_pWanted->GetWantedLevel();
	if(CoopSharedWanted){
		if(leadLevel > s_leadWanted || partnerLevel > s_partnerWanted){
			const int32 level = Max(leadLevel, partnerLevel);
			if(leadLevel != level)
				lead->m_pWanted->SetWantedLevel(level);
			if(partnerLevel != level)
				partner->m_pWanted->SetWantedLevel(level);
		}
	}else{
		// With the option off the pair does not share one level: player 1's
		// crimes are not the partner's, and the partner's record is their own.
		// But the law is still the pair's problem.  Every cop in the city reads
		// player 1's wanted level and nobody else's, so a partner's crime the
		// police never hear about is a crime with no consequence at all.  A
		// rise of theirs is therefore put on player 1 -- the stars and the
		// chase both follow -- while a rise of his is not put on them.
		if(partnerLevel > s_partnerWanted && partnerLevel > leadLevel)
			lead->m_pWanted->SetWantedLevel(partnerLevel);
	}
	s_leadWanted = lead->m_pWanted->GetWantedLevel();
	s_partnerWanted = partner->m_pWanted->GetWantedLevel();
}

void
ForgetArsenal(void)
{
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		s_seenType[slot] = WEAPONTYPE_UNARMED;
		s_owedType[slot] = WEAPONTYPE_UNARMED;
		s_poolAmmo[slot] = -1;
	}
}

// Forget the synced pools without forgetting the weapons.  For a partner that
// is being replaced: the new ped's counts are theirs to start, and the first
// sync takes them from the pool rather than measuring a spend against a stale
// figure.
void
ForgetSharedAmmo(void)
{
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++)
		s_poolAmmo[slot] = -1;
}

bool
MirrorsWeapon(eWeaponType type)
{
	// The detonator is handed out by the throw that needs it (CWeapon::Fire), and
	// the camera is a mission prop.
	return type != WEAPONTYPE_UNARMED && type != WEAPONTYPE_DETONATOR && type != WEAPONTYPE_CAMERA;
}

void
UpdateArsenal(CPlayerPed *lead, CPlayerPed *partner)
{
	// A rampage puts its own weapon into one of player 1's slots and takes it
	// out again when it ends.  That slot is not looked at while it lasts:
	// copying it would leave the partner holding a minigun with thirty thousand
	// rounds once it was over, and noting it would make player 1's own weapon
	// coming back afterwards look like a new one.
	const int frenzySlot = CDarkel::GetFrenzyWeaponSlot();

	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		const eWeaponType type = lead->GetWeapon(slot).m_eWeaponType;
		if(slot != frenzySlot){
			if(MirrorsWeapon(type) && type != s_seenType[slot]){
				// New to player 1, so new to the partner.  Also how a partner
				// who has just arrived gets everything at once: nothing has
				// been seen yet, so every slot is new.
				//
				// The ammo is not handed over with it; the pool is.  The weapon
				// goes over empty and SyncSharedAmmo fills it from the pool on
				// the same frame.
				s_owedType[slot] = type;
			}
			s_seenType[slot] = type;
		}

		if(s_owedType[slot] == WEAPONTYPE_UNARMED)
			continue;
		// The same wait CPed::RequestDelayedWeapon does, and for the same reason:
		// a weapon given before its model has loaded is a weapon with nothing to
		// draw.  Player 1's current gun is always in memory; the rest of what
		// they carry may not be.
		const CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(s_owedType[slot]);
		const int32 model1 = info->m_nModelId;
		const int32 model2 = info->m_nModel2Id;
		if(model1 != -1)
			CStreaming::RequestModel(model1, STREAMFLAGS_DEPENDENCY);
		if(model2 != -1)
			CStreaming::RequestModel(model2, STREAMFLAGS_DEPENDENCY);
		if((model1 == -1 || CStreaming::HasModelLoaded(model1)) &&
		   (model2 == -1 || CStreaming::HasModelLoaded(model2))){
			partner->GiveWeapon(s_owedType[slot], 0, true);
			s_owedType[slot] = WEAPONTYPE_UNARMED;
			// A different weapon in the slot is a different pool for it.
			s_poolAmmo[slot] = -1;
		}
	}

	// Draw the weapon they were holding, or failing that the one player 1 is,
	// as soon as it has arrived.  Asked for the way a pickup asks: by naming the
	// slot and letting CPlayerPed::ProcessWeaponSwitch make the change.
	if(s_wantSlot >= 0){
		if(s_wantSlot == WEAPONSLOT_UNARMED || partner->HasWeaponSlot(s_wantSlot)){
			partner->m_nSelectedWepSlot = s_wantSlot;
			s_wantSlot = -1;
		}else if(CTimer::GetTimeInMilliseconds() > s_wantSlotUntil)
			s_wantSlot = -1;
	}
}

// One ammo count per slot, shared by the two players.  Player 1's m_nAmmoTotal
// is the pool; the partner's is a cache of it.  What the partner spent last
// frame is measured against the cache here, at the top of the frame, and taken
// out of the pool -- so a burst from one empties the other too, and the same
// rounds cannot be fired twice.
//
// The clip is each player's own and is clamped to what the pool has left: a
// magazine cannot hold rounds the other player has already spent.  The reload
// timer is untouched, so a reload still takes its own time.
void
SyncSharedAmmo(CPlayerPed *lead, CPlayerPed *partner)
{
	const int frenzySlot = CDarkel::GetFrenzyWeaponSlot();

	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		CWeapon &leadWeapon = lead->GetWeapon(slot);
		CWeapon &partnerWeapon = partner->GetWeapon(slot);
		// Only a weapon both of them hold, and never the rampage's, which is
		// player 1's alone and must not arm the partner when it ends.
		if(slot == frenzySlot || !MirrorsWeapon(leadWeapon.m_eWeaponType) ||
		   partnerWeapon.m_eWeaponType != leadWeapon.m_eWeaponType){
			s_poolAmmo[slot] = -1;
			continue;
		}

		if(s_poolAmmo[slot] < 0){
			// First frame both hold this weapon: the partner starts from the
			// pool rather than from whatever their own count happened to be.
			s_poolAmmo[slot] = leadWeapon.m_nAmmoTotal;
		}else{
			const int32 spent = s_poolAmmo[slot] - partnerWeapon.m_nAmmoTotal;
			if(spent > 0)
				leadWeapon.m_nAmmoTotal = Max(0, leadWeapon.m_nAmmoTotal - spent);
		}
		partnerWeapon.m_nAmmoTotal = leadWeapon.m_nAmmoTotal;
		if(leadWeapon.m_nAmmoInClip > leadWeapon.m_nAmmoTotal)
			leadWeapon.m_nAmmoInClip = leadWeapon.m_nAmmoTotal;
		if(partnerWeapon.m_nAmmoInClip > partnerWeapon.m_nAmmoTotal)
			partnerWeapon.m_nAmmoInClip = partnerWeapon.m_nAmmoTotal;
		// A weapon the partner emptied and left OUT_OF_AMMO comes back to life
		// when the pool is refilled.  Player 1's own is reset by CPed::GiveWeapon
		// when the pickup lands; the partner is not given anything, so it has to
		// be done here or they would never be able to fire it again.
		if(partnerWeapon.m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO && partnerWeapon.m_nAmmoTotal > 0)
			partnerWeapon.m_eWeaponState = WEAPONSTATE_READY;
		s_poolAmmo[slot] = partnerWeapon.m_nAmmoTotal;
	}
}

// --- putting the partner into the world, and taking them out -----------------
// Somewhere to stand next to a point: beside it first, then behind, then in
// front.  A spot has to be on the same level, not through a wall from where it
// was measured from, and not inside anything.
bool
FindSpotBeside(const CVector &base, const CVector &right, const CVector &forward, float spacing, CVector &out)
{
	static const float kOffsets[][2] = {
		{ 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, -1.0f }, { 1.0f, -1.0f },
		{ -1.0f, -1.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
	};
	CVector2D side(right.x, right.y);
	CVector2D ahead(forward.x, forward.y);
	if(side.MagnitudeSqr() < 0.01f || ahead.MagnitudeSqr() < 0.01f){
		// Something on its side or its nose has no "beside" worth the name.
		side = CVector2D(1.0f, 0.0f);
		ahead = CVector2D(0.0f, 1.0f);
	}
	side.Normalise();
	ahead.Normalise();

	for(uint32 i = 0; i < ARRAY_SIZE(kOffsets); i++){
		CVector pos = base;
		pos.x += (side.x*kOffsets[i][0] + ahead.x*kOffsets[i][1])*spacing;
		pos.y += (side.y*kOffsets[i][0] + ahead.y*kOffsets[i][1])*spacing;
		if(!CPedPlacement::FindZCoorForPed(&pos))
			continue;
		if(Abs(pos.z - base.z) > 1.5f)
			continue;
		if(!CWorld::GetIsLineOfSightClear(base, pos, true, false, false, true, false, false, false))
			continue;
		if(CWorld::TestSphereAgainstWorld(pos, 0.35f, nil, true, true, false, true, false, false) != nil)
			continue;
		out = pos;
		return true;
	}
	return false;
}

// Whether the partner can sit in this vehicle while player 1 drives it.  Only
// ever a vehicle player 1 is driving: the engine makes any player who gets onto
// a bike or into a boat its driver whatever seat they asked for, and a car with
// a player in it and nobody at the wheel is not one the traffic code expects.
bool
CanRideAlong(CPlayerPed *lead, CVehicle *vehicle)
{
	if(vehicle == nil || vehicle->pDriver != lead)
		return false;
	if(lead->m_nPedState != PED_DRIVING || lead->m_objective == OBJECTIVE_LEAVE_CAR)
		return false;
	if(!vehicle->IsCar() && !vehicle->IsBike())
		return false;
	if(vehicle->GetStatus() == STATUS_WRECKED || vehicle->bIsInWater || vehicle->IsUpsideDown())
		return false;
	for(int i = 0; i < vehicle->m_nNumMaxPassengers; i++)
		if(vehicle->pPassengers[i] == nil)
			return true;
	return false;
}

// Straight into a seat, the way COMMAND_CREATE_CHAR_AS_PASSENGER seats a ped
// it has just made.  Deliberately not CPed::WarpPedIntoCar, which marks the car
// as the player's to drive for any ped that IsPlayer().
bool
SeatPartner(CPlayerPed *partner, CVehicle *vehicle)
{
	if(!vehicle->AddPassenger(partner))
		return false;
	if(vehicle->bIsBus)
		partner->bRenderPedInCar = false;
	partner->m_pMyVehicle = vehicle;
	partner->m_pMyVehicle->RegisterReference((CEntity**)&partner->m_pMyVehicle);
	partner->bInVehicle = true;
	partner->SetPedState(PED_DRIVING);
	partner->bUsesCollision = false;
	partner->AddInCarAnims(vehicle, false);
	return true;
}

void
RemovePartner(const char *why, bool carry)
{
	CPlayerPed *partner = CWorld::Players[PARTNER].m_pPed;
	s_downTime = 0;
	s_reviveTime = 0;
	// A revive that has not spawned yet dies with the partner: the next arrival
	// is beside player 1 again, not at a body that is no longer there.  This is
	// what keeps a session pause -- a mission, player 1 going down -- between
	// the revive and the spawn from leaving the position behind.
	s_reviveValid = false;
	s_aim[PARTNER].active = false;
	s_aim[PARTNER].drawReticle = false;
	if(partner == nil)
		return;

	s_carry.valid = carry && !partner->DyingOrDead();
	if(s_carry.valid){
		s_carry.health = partner->m_fHealth;
		s_carry.armour = partner->m_fArmour;
		s_carry.slot = partner->m_nSelectedWepSlot;
		for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++)
			s_carry.weapon[slot] = partner->GetWeapon(slot).m_eWeaponType;
	}

	// The destructor below gives back the one door the ped was using.  Stepping
	// off the back of a bike holds two, and the one left behind would keep that
	// bike from ever being cleared away or ridden pillion again.
	if(partner->m_nPedState == PED_EXIT_CAR && partner->m_pMyVehicle != nil && partner->m_pMyVehicle->IsBike())
		partner->m_pMyVehicle->m_nGettingOutFlags &= ~(CAR_DOOR_FLAG_LR | CAR_DOOR_FLAG_RR);

	COOP_LOG("WII coop: player 2 out (%s)\n", why);
	// The destructor does the rest: out of the world, out of whatever seat or
	// doorway they were in, fire out, weapons freed.  It is the same path the
	// engine takes for any ped it removes, which is the point of deleting rather
	// than hiding -- there is no half-present player for a pool scan to find.
	CWorld::RemoveReferencesToDeletedObject(partner);
	delete partner;
	CWorld::Players[PARTNER].m_pPed = nil;
}

// Whether another player is close enough to a downed partner to bring them
// back.  What has to be reached is the body -- or the car, for someone at
// their last point of health in one, who is not dead yet.
bool
SomeoneCanRevive(CPlayerPed *partner)
{
	const CVector at = partner->bInVehicle && partner->m_pMyVehicle != nil ?
		partner->m_pMyVehicle->GetPosition() : partner->GetPosition();
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *other = CWorld::Players[i].m_pPed;
		if(other == nil || other == partner || other->DyingOrDead())
			continue;
		const CVector pos = other->bInVehicle && other->m_pMyVehicle != nil ?
			other->m_pMyVehicle->GetPosition() : other->GetPosition();
		if((pos - at).Magnitude() <= kReviveRange)
			return true;
	}
	return false;
}

bool
SpawnPartner(CPlayerPed *lead)
{
	// Leave the ped pool some room: the population code starts deleting
	// pedestrians when fewer than eight slots are free.
	if(CPools::GetPedPool()->GetNoOfFreeSpaces() < 10)
		return false;
	// Between CPed::Undress and CPed::Dress the player model is not there to
	// make a second ped out of.
	if(lead->m_rwObject == nil || !CStreaming::HasModelLoaded(MI_PLAYER) ||
	   CModelInfo::GetModelInfo(MI_PLAYER)->GetRwObject() == nil)
		return false;

	CVehicle *ride = nil;
	CVector pos;
	if(s_reviveValid){
		// Brought back where they fell, not beside player 1 and not into their
		// car: the ground the fight was on is the point of a revive.
		pos = s_reviveAt;
		s_reviveValid = false;
	}else if(lead->bInVehicle && lead->m_pMyVehicle){
		CVehicle *vehicle = lead->m_pMyVehicle;
		if(CanRideAlong(lead, vehicle)){
			ride = vehicle;
			pos = vehicle->GetPosition();
		}else{
			// No seat.  Beside it if it is standing still, and otherwise not
			// yet: they join when player 1 stops or gets out.
			if(vehicle->IsBoat() || vehicle->m_vecMoveSpeed.MagnitudeSqr() > SQR(kSlowVehicle))
				return false;
			if(!FindSpotBeside(vehicle->GetPosition(), vehicle->GetRight(), vehicle->GetForward(), 3.0f, pos))
				return false;
		}
	}else{
		if(!FindSpotBeside(lead->GetPosition(), lead->GetRight(), lead->GetForward(), 1.6f, pos))
			return false;
	}

	// Making a player ped resets the things that were only ever reset when THE
	// player was made: among them the speed the game runs at, which a cheat or
	// a script may have changed and would find put back every time the partner
	// came round a corner.
	const float timeScale = CTimer::GetTimeScale();
	CPlayerPed *partner = new CPlayerPed();
	CTimer::SetTimeScale(timeScale);
	// The characters are special models, not model slots of their own: the name
	// is loaded into the slot that goes with its number in the game's own list
	// (CStreaming::RequestSpecialChar), the same slots the cutscenes spawn their
	// characters from -- which is safe here because a cutscene, like a mission,
	// stands co-op down and takes the partner with it.  Looking the name up as a
	// model finds nothing, which is how a chosen skin used to come out as
	// player 1.
	//
	// The names are the character models in the game's own archive (the ig*
	// entries in gta3.dir) and the slots are ones its scripts leave alone; each
	// skin has one of its own, so two partners never share a slot.
	static const char *const kCoopSkinModels[] = { nil,
		"igcandy", "igken", "igbuddy", "igphil", "igdiaz", "igmerc",
		"igsonny", "igcolon", "igjezz", "ighlary", "iggonz" };
	static const int kCoopSkinChars[] = { -1,
		12, 11, 14, 17, 20, 18,
		13, 15, 16, 19, 10 };
	if(WiiCoopSkin > 0 && WiiCoopSkin < (int)ARRAY_SIZE(kCoopSkinChars) && kCoopSkinChars[WiiCoopSkin] >= 0){
		const int charId = kCoopSkinChars[WiiCoopSkin];
		const int skinModel = MI_SPECIAL01 + charId;
		CStreaming::RequestSpecialChar(charId, kCoopSkinModels[WiiCoopSkin], STREAMFLAGS_DEPENDENCY);
		CStreaming::LoadAllRequestedModels(false);
		if(CStreaming::HasModelLoaded(skinModel)){
			// The model brings an animation group of its own.  The player's is
			// what the controls and the weapon animations are written against,
			// and STILLLIKEDRESSINGUP keeps it for the same reason.
			AssocGroupId animGroup = partner->m_animGroup;
			// Same sequence the cheat uses to change a player's model: the old
			// clump has to go and the model index has to be invalid before
			// SetModelIndex, or the ped keeps the clump it was built with.
			partner->DeleteRwObject();
			partner->m_modelIndex = -1;
			partner->SetModelIndex(skinModel);
			partner->m_animGroup = animGroup;
		}
	}
	// Not PEDTYPE_PLAYER1, and that is what keeps this ped out of the save:
	// CPools::SavePedPool picks what to write by exactly that type, and
	// LoadPedPool would hand a second one to CWorld::Players[0].  It still
	// answers true to IsPlayer(), which is what the rest of the engine asks.
	partner->m_nPedType = PEDTYPE_PLAYER2;
	// What COMMAND_CREATE_PLAYER does for player 1, and what protects them: a
	// RANDOM_CHAR is fair game for CPopulation::RemovePedsIfThePoolGetsFull, and
	// a vehicle with only RANDOM_CHARs aboard is one the engine may delete.
	partner->CharCreatedBy = MISSION_CHAR;
	// Stay in the car when player 1 gets back into it, instead of being told to
	// get out like any other passenger.  They leave when they press the button.
	partner->bStayInCarOnJack = true;
	// Nor pulled out of it: player 1 coming to the passenger door of a car with
	// the partner already in that seat would otherwise haul them into the road
	// to get past.
	partner->bDontDragMeOutCar = true;

	CWorld::Players[PARTNER].m_pPed = partner;
	partner->RegisterReference((CEntity**)&CWorld::Players[PARTNER].m_pPed);

	partner->SetOrientation(0.0f, 0.0f, 0.0f);
	partner->SetPosition(pos);
	partner->m_fRotationCur = lead->m_fRotationCur;
	partner->m_fRotationDest = lead->m_fRotationCur;
	partner->SetHeading(lead->m_fRotationCur);
	partner->m_area = lead->m_area;
	partner->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	CWorld::Add(partner);
	partner->m_wepAccuracy = 100;

	if(s_carry.valid){
		partner->m_fHealth = s_carry.health;
		partner->m_fArmour = s_carry.armour;
		s_wantSlot = s_carry.slot;
		// Their weapons come back through the same queue player 1's gains do,
		// on top of anything that was still on its way to them when they went.
		// Empty, because the ammo is the shared pool, not a count the partner
		// owns; the first sync fills them from it.
		for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
			if(s_carry.weapon[slot] == WEAPONTYPE_UNARMED)
				continue;
			if(s_owedType[slot] == WEAPONTYPE_UNARMED)
				s_owedType[slot] = s_carry.weapon[slot];
		}
	}else{
		partner->m_fHealth = CWorld::Players[LEAD].m_nMaxHealth;
		partner->m_fArmour = lead->m_fArmour;
		s_wantSlot = lead->m_currentWeapon;
		ForgetArsenal();
	}
	s_carry.valid = false;
	// The new ped's ammo counts start from the pool, not from a spend measured
	// against the last partner's.
	ForgetSharedAmmo();
	// The pair shares its heat: a partner who arrives while player 1 is wanted
	// is wanted too, and the mirror starts from there.  With the option off
	// they arrive clean and keep their own.
	if(CoopSharedWanted){
		partner->m_pWanted->SetWantedLevel(lead->m_pWanted->GetWantedLevel());
		s_leadWanted = lead->m_pWanted->GetWantedLevel();
		s_partnerWanted = partner->m_pWanted->GetWantedLevel();
	}
	s_wantSlotUntil = CTimer::GetTimeInMilliseconds() + 4000;

	if(ride != nil && !SeatPartner(partner, ride)){
		// CanRideAlong found a seat a moment ago, so this cannot happen; if it
		// ever does, a player standing inside a car is not the way to find out.
		RemovePartner("no seat after all", false);
		return false;
	}

	s_aim[PARTNER].facing = false;
	s_aim[PARTNER].assistHandle = -1;
	COOP_LOG("WII coop: player 2 in%s\n", ride != nil ? " (riding with player 1)" : "");
	if(s_announceJoin){
		s_announceJoin = false;
		Tell("WII_P2I");
	}
	return true;
}

// --- what stops a session ----------------------------------------------------
// Returns nil when co-op may run, and otherwise what is in the way.
const char *
SessionBlocker(void)
{
	if(!CCamera::bWiiCoopCamera)
		return "switched off";
	CPlayerPed *lead = CWorld::Players[LEAD].m_pPed;
	if(CWorld::PlayerInFocus != LEAD || lead == nil)
		return "no player";
	// The boundary the whole design rests on.  Mission scripts, their cutscene
	// cameras and their fail states were all written for one player, and none of
	// them run while this is false.
	if(CTheScripts::IsPlayerOnAMission())
		return "mission";
	// IsCutsceneProcessing as well: it is true from the moment one starts to
	// load, which can be a couple of seconds before it is running.
	if(CCutsceneMgr::IsRunning() || CCutsceneMgr::IsCutsceneProcessing() || TheCamera.m_WideScreenOn)
		return "cutscene";
	if(CReplay::IsPlayingBack())
		return "replay";
	if(CWorld::Players[LEAD].m_WBState != WBSTATE_PLAYING || lead->DyingOrDead() ||
	   lead->m_nPedState == PED_ARRESTED)
		return "player 1 down";
	return nil;
}

// The nearest car, bike or boat the partner could take as its driver.  All
// three come off the driver's own pad now; a bike makes whoever climbs on it
// the rider, and a boat makes whoever boards it the driver, which is what the
// partner wants in every case.  Never one a player is driving -- that would be
// a carjack of player 1 -- and not one player 1 is already walking to, a wreck
// or one on its roof, which nobody can enter.
CVehicle *
FindCarToSteal(CPlayerPed *lead, CPlayerPed *partner)
{
	CVehicle *best = nil;
	float bestDist = SQR(kStealRange);
	const CVector pos = partner->GetPosition();
	for(int i = CPools::GetVehiclePool()->GetSize() - 1; i >= 0; i--){
		CVehicle *vehicle = CPools::GetVehiclePool()->GetSlot(i);
		if(vehicle == nil || (!vehicle->IsCar() && !vehicle->IsBike() && !vehicle->IsBoat()))
			continue;
		if(vehicle->GetStatus() == STATUS_WRECKED || vehicle->IsUpsideDown())
			continue;
		// A boat is in the water by definition; a car in the water is sunk.
		if(!vehicle->IsBoat() && vehicle->bIsInWater)
			continue;
		if(vehicle->pDriver != nil && vehicle->pDriver->IsPlayer())
			continue;
		// The one player 1 is entering: two players finishing the same car's
		// enter as its driver would leave one of them in the seat with the
		// other's ped as the car's driver.  Tested on the state, not on
		// m_carInObjective alone: the field is left pointing at the last car
		// player 1 entered -- the game clears it only through ClearObjective,
		// which the player's own enter never goes through -- so a car they had
		// driven once could never be taken by the partner again.
		if((lead->m_nPedState == PED_ENTER_CAR || lead->m_nPedState == PED_CARJACK) &&
		   vehicle == lead->m_carInObjective)
			continue;
		const float dist = (vehicle->GetPosition() - pos).MagnitudeSqr();
		if(dist < bestDist){
			bestDist = dist;
			best = vehicle;
		}
	}
	return best;
}

// --- driving and riding along ------------------------------------------------
// CPlayerInfo::Process is where player 1's enter/exit button is read, and it is
// only ever run for the player in focus.  This is the partner's share of it.
// On foot the button means one of two things, in this order:
//
//   * a free seat in whatever player 1 is driving or climbing into -- get in
//     beside them, which is what it has always meant; or
//   * failing that, the nearest car within reach -- take it as its driver and
//     drive it.  That is the partner's own car, not player 1's.
//
// Returns true when the partner should be put straight into a seat instead: the
// car is already moving, and nobody catches a moving car on foot.
bool
UpdatePartnerVehicle(CPlayerPed *lead, CPlayerPed *partner, CPad *pad)
{
	if(partner->bInVehicle){
		CVehicle *vehicle = partner->m_pMyVehicle;
		if(vehicle == nil || partner->m_nPedState != PED_DRIVING)
			return false;
		// Out once it has stopped, or bailing out while it moves if the vehicle
		// allows it.  CPlayerInfo::Process only lets a DRIVER bail out of a car,
		// because the only passenger seat a player ever had was a taxi's; but
		// CPed::SetExitCar rolls any player out of any door, and a partner who
		// could not leave until player 1 chose to stop would be a hostage.
		//
		// These are the tests SetExitCar itself makes, and the order is only
		// given when it would be obeyed.  One it refuses is not dropped: it
		// stays with the ped, is tried again every five seconds, and in the
		// meantime marks them as a hostage -- so a press that came a moment too
		// early used to lock the door for five seconds, and then throw the
		// partner out later, unasked, if the car happened to be going at a
		// speed to bail out from when the next try came round.
		const bool canLeave = vehicle->CanPedExitCar(false) ||
			(vehicle->IsBike() ? vehicle->CanPedJumpOffBike() : vehicle->CanPedJumpOutCar());
		if(partner->m_objective == OBJECTIVE_LEAVE_CAR){
			// The same goes for an order that came from somewhere else (a car
			// on fire gives one) and could not be carried out yet.
			if(!canLeave){
				partner->SetObjective(OBJECTIVE_NONE);
				partner->m_leaveCarTimer = 0;
			}
			return false;
		}
		const bool wantsOut = partner->bVehExitWillBeInstant ? pad->ExitVehicleJustDown() : pad->GetExitVehicle();
		if(wantsOut && canLeave && vehicle->GetStatus() != STATUS_WRECKED && !vehicle->bIsInWater)
			partner->SetObjective(OBJECTIVE_LEAVE_CAR, vehicle);
		return false;
	}

	if(!pad->ExitVehicleJustDown() || !partner->IsPedInControl())
		return false;

	// A seat with player 1 first.  The car they are in, or the one they are
	// climbing into, and only if it is theirs or empty and has room.
	CVehicle *vehicle = nil;
	if(lead->bInVehicle)
		vehicle = lead->m_pMyVehicle;
	else if(lead->m_nPedState == PED_ENTER_CAR || lead->m_nPedState == PED_CARJACK)
		vehicle = lead->m_carInObjective;
	if(vehicle != nil && (vehicle->pDriver == nil || vehicle->pDriver == lead) &&
	   (vehicle->IsCar() || vehicle->IsBike()) &&
	   vehicle->GetStatus() != STATUS_WRECKED && !vehicle->bIsInWater &&
	   (vehicle->GetPosition() - partner->GetPosition()).Magnitude() <= kBoardingRange){
		bool seat = false;
		for(int i = 0; i < vehicle->m_nNumMaxPassengers; i++)
			if(vehicle->pPassengers[i] == nil)
				seat = true;
		if(seat){
			// Standing still, or nearly: walk to a door and get in like anybody
			// else.  Already rolling: straight into the seat.
			//
			// Onto a bike it is always straight into the seat, and only once
			// player 1 is sitting on it.  The engine makes a bike the player's
			// to ride when any player finishes climbing on, whichever seat they
			// asked for -- so a partner who was still walking over when player 1
			// got off again would end up on the back of a bike with nobody on
			// the front, being ridden from player 1's pad, and the bike's
			// drive-by code looks up its rider without asking whether there is
			// one.
			if(vehicle->IsBike())
				return CanRideAlong(lead, vehicle);
			if(vehicle->m_vecMoveSpeed.MagnitudeSqr() > SQR(0.04f) && CanRideAlong(lead, vehicle))
				return true;
			partner->SetObjective(OBJECTIVE_ENTER_CAR_AS_PASSENGER, vehicle);
			return false;
		}
	}

	// No seat with player 1.  Take a car of their own instead.
	CVehicle *steal = FindCarToSteal(lead, partner);
	if(steal != nil)
		partner->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, steal);
	return false;
}

// Whether the partner has to be brought back to player 1.
const char *
NeedsRegroup(CPlayerPed *lead, CPlayerPed *partner, bool leadTeleported)
{
	if(leadTeleported)
		return "player 1 was moved";

	CVehicle *partnerVehicle = partner->bInVehicle ? partner->m_pMyVehicle : nil;
	// Someone else at the wheel.  If the partner is driving their own car that
	// is the partner, not somebody else, and the leash below is what keeps them
	// near player 1.
	if(partnerVehicle != nil && partnerVehicle->pDriver != nil &&
	   partnerVehicle->pDriver != lead && partnerVehicle->pDriver != partner)
		// Somebody else is at the wheel.  Riding along means with player 1.
		return "driven off by someone else";

	const CVector leadPos = (lead->bInVehicle && lead->m_pMyVehicle) ? lead->m_pMyVehicle->GetPosition() : lead->GetPosition();
	const CVector partnerPos = partnerVehicle != nil ? partnerVehicle->GetPosition() : partner->GetPosition();
	if(partnerPos.z < leadPos.z - kFallLimit)
		return "fell";
	// A partner at the wheel of their own car gets the longer leash; everybody
	// else, including a passenger in player 1's car, gets the walking one.
	const float leash = (partnerVehicle != nil && partnerVehicle->pDriver == partner) ? kLeashDriving : kLeash;
	if((leadPos - partnerPos).Magnitude() > leash)
		return "too far from player 1";
	return nil;
}

// --- aiming ------------------------------------------------------------------
bool
AssistWanted(CPlayerPed *ped, CPad *pad)
{
#ifdef AIM_ASSIST
	if(!CAimAssist::bEnabled)
		return false;
#endif
	// The same conditions as CAimAssist: firing or aiming, with a gun that can
	// be aimed, and not while running flat out.
	if(!(pad->GetWeapon() || pad->GetTarget()))
		return false;
	if(ped->m_nSelectedWepSlot != ped->m_currentWeapon || ped->m_nMoveState == PEDMOVE_SPRINT)
		return false;
	const CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(ped->GetWeapon()->m_eWeaponType);
	return info->m_nWeaponSlot > WEAPONSLOT_PROJECTILE && info->IsFlagSet(WEAPONFLAG_CANAIM);
}

// The live ped nearest to being in line with where the player is aiming, if
// one is close enough to in line to count.
CPed *
FindAssistTarget(CPlayerPed *ped, const CVector &origin, float heading, float range)
{
	const float dirX = -Sin(heading);
	const float dirY = Cos(heading);
	CPed *best = nil;
	float bestRatio = 1.0f;

	for(int i = CPools::GetPedPool()->GetSize() - 1; i >= 0; i--){
		CPed *other = CPools::GetPedPool()->GetSlot(i);
		// The rules of the Classic lock-on (CPlayerPed::FindWeaponLockOnTarget),
		// plus one: never the other player.
		if(other == nil || other == ped || other->IsPlayer() || other->DyingOrDead() ||
		   other->m_leader == ped || other->bNeverEverTargetThisPed)
			continue;
		if(other->bInVehicle && !(other->m_pMyVehicle && other->m_pMyVehicle->IsBike()))
			continue;

		const CVector to = other->GetPosition() - origin;
		const float dist = to.Magnitude2D();
		if(dist < 1.0f || dist > range || Abs(to.z) > dist)
			continue;
		const float cosAngle = (to.x*dirX + to.y*dirY)/dist;
		if(cosAngle < 0.95f)
			continue;
		const float cone = Clamp(Atan2(kAssistRadius, dist), kAssistMinCone, kAssistMaxCone);
		const float ratio = Acos(Min(cosAngle, 1.0f))/cone;
		// On screen only.  A rifle reaches a good deal further than the shared
		// camera sees, and a gun that swings round to someone neither player can
		// see is not helping anybody aim.
		if(ratio < bestRatio && other->GetIsOnScreen()){
			best = other;
			bestRatio = ratio;
		}
	}
	return best;
}

void
UpdateAim(int index)
{
	CoopAim &aim = s_aim[index];
	CPlayerPed *ped = CWorld::Players[index].m_pPed;
	const bool wasActive = aim.active;
	aim.active = false;
	aim.hasTarget = false;
	aim.engaged = false;
	aim.drawReticle = false;
	if(ped == nil || !CCoop::UsesReticleAim())
		return;
	if(ped->DyingOrDead())
		return;
	// Inside a vehicle the driver aims, and so does a passenger with a window
	// or a side to shoot out of -- a car's seat (CAutomobile's drive-by), a
	// bike's pillion (CBike's), or an unarmed helicopter's open cabin, whose
	// passenger has the drive-by.  An armed helicopter's passenger waits for
	// the mounted gun; a plane's or a boat's has no drive-by to aim.
	if(ped->bInVehicle){
		CVehicle *vehicle = ped->m_pMyVehicle;
		if(vehicle == nil)
			return;
		if(vehicle->pDriver != ped){
			const bool gunSeat =
				(vehicle->IsCar() && !vehicle->IsRealPlane() &&
				 vehicle->GetModelIndex() != MI_HUNTER && vehicle->GetModelIndex() != MI_SEASPAR) ||
				vehicle->IsBike();
			if(!gunSeat)
				return;
		}
	}

	const uint32 now = CTimer::GetTimeInMilliseconds();
	CPad *pad = GetPadFromPlayer(ped);
	const CVector origin = ped->GetPosition();

	// The right stick is a direction on the screen, and so a direction in the
	// world once the camera's heading is taken out of it -- the same sum
	// CPlayerPed::PlayerControlZelda does for the left one.
	const float stickX = pad->NewState.RightStickX;
	const float stickY = pad->NewState.RightStickY;
	const bool pushed = !pad->ArePlayerControlsDisabled() && stickX*stickX + stickY*stickY > SQR(kStickAimThreshold);
	if(pushed){
		aim.stickHeading = CGeneral::LimitRadianAngle(
			CGeneral::GetRadianAngleBetweenPoints(0.0f, 0.0f, -stickX, stickY) - TheCamera.Orientation);
		aim.stickTime = now;
		aim.stickSeen = true;
	}
	if(!wasActive)
		aim.rawHeading = ped->m_fRotationCur;

	// An aim does not lapse in the middle of being used.  While a player is
	// firing, a direction that is no longer being given -- the stick let go so
	// the same thumb can hold the trigger, the pointer wandered off the screen
	// -- is held where it was, and a player who never gave one at all is taken
	// to be aiming the way they face.  Otherwise the aim would run out part of
	// the way through a burst and the controls would change under the player's
	// hands, to ones that will not let them turn or walk while they fire.
	// Guns and things that are thrown only: fists and bats are not aimed.
	const bool pointing = aim.pointerSeen && now - aim.pointerTime <= kPointerFreshMs;
	const bool melee = CWeaponInfo::GetWeaponInfo(ped->GetWeapon()->m_eWeaponType)->m_eWeaponFire == WEAPON_FIRE_MELEE;
	const bool firing = !melee && (pad->GetWeapon() || ped->m_nPedState == PED_ATTACK || ped->m_nPedState == PED_AIM_GUN);
	if(firing && !pointing && !pushed){
		aim.stickHeading = aim.rawHeading;
		aim.stickTime = now;
		aim.stickSeen = true;
	}

	if(pointing){
		// A pointer.  Follow the ray under it into the world.
		const CVector camPos = TheCamera.GetPosition();
		const CVector ray = CCamera::FindCrosshairRay(TheCamera.GetForward(), TheCamera.GetUp(),
			CDraw::GetFOV(), aim.pointerX, aim.pointerY);

		// Where the ray comes down to the player's own chest height.  A camera
		// that looks down always has such a point; if this one somehow does not,
		// there is nothing to aim at.
		if(ray.z > -0.05f)
			return;
		const float toLevel = (origin.z - camPos.z)/ray.z;

		CColPoint col;
		CEntity *hit = nil;
		CWorld::pIgnoreEntity = ped;
		const bool found = CWorld::ProcessLineOfSight(camPos, camPos + ray*Min(kReticleRay, toLevel + kReticleRayPast),
			col, hit, true, true, true, true, false, false, false);
		CWorld::pIgnoreEntity = nil;

		if(found && hit != nil && (hit->IsPed() || hit->IsVehicle()) && !IsAnyPlayerPed(hit)){
			// On something that can be shot: aim at the very spot.
			aim.point = col.point;
			aim.hasTarget = true;
			aim.engaged = hit->IsPed() && !((CPed*)hit)->DyingOrDead();
		}else{
			// On the scenery.  What the player means then is a direction, and the
			// ground under the reticle is the wrong way to get one: seen from
			// above and behind, a reticle laid over someone's chest lands on the
			// ground a good metre past their feet, and from the side that is
			// enough to miss by.  So the level plane at the player's own chest
			// height is used instead, where a reticle on a chest and the chest
			// itself are the same place.
			aim.point = camPos + ray*toLevel;
		}
		aim.reticleX = aim.pointerX;
		aim.reticleY = aim.pointerY;
		aim.drawReticle = true;
	}else if(aim.stickSeen && now - aim.stickTime <= kStickHoldMs){
		aim.point = origin + CVector(-Sin(aim.stickHeading), Cos(aim.stickHeading), 0.0f)*kStickReach;
		// Nothing put this reticle on the screen, so work out where it is.
		CVector screen;
		float w, h;
		if(CSprite::CalcScreenCoors(aim.point, &screen, &w, &h, false)){
			aim.reticleX = screen.x/SCREEN_WIDTH;
			aim.reticleY = screen.y/SCREEN_HEIGHT;
			aim.drawReticle = true;
		}
	}else
		return;

	aim.active = true;
	const CVector toPoint = aim.point - origin;
	if(toPoint.Magnitude2D() > kMinAimDistance)
		aim.rawHeading = Atan2(-toPoint.x, toPoint.y);
	else if(!aim.hasTarget)
		// Aimed at their own feet.  Keep facing wherever they last aimed.
		aim.point = origin + CVector(-Sin(aim.rawHeading), Cos(aim.rawHeading), 0.0f)*kStickReach;
	aim.heading = aim.rawHeading;

	if(aim.hasTarget || !AssistWanted(ped, pad)){
		if(!aim.hasTarget)
			aim.assistHandle = -1;
		return;
	}
	const float range = CWeaponInfo::GetWeaponInfo(ped->GetWeapon()->m_eWeaponType)->m_fRange;
	CPed *target = FindAssistTarget(ped, origin, aim.heading, range);
	if(target == nil){
		aim.assistHandle = -1;
		return;
	}
	CVector chest;
	target->m_pedIK.GetComponentPosition(chest, PED_MID);
	// That is read from the skeleton, which is only as fresh as the last time
	// the ped was drawn.  If it is nowhere near them, it was not lately.
	if((chest - target->GetPosition()).MagnitudeSqr() > SQR(1.5f))
		chest = target->GetPosition();
	// Only what a shot could reach, rechecked now and then: it is a world ray.
	const int32 handle = CPools::GetPedPool()->GetIndex(target);
	if(handle != aim.assistHandle || now - aim.assistSightTime > kAssistSightMs){
		aim.assistVisible = CWorld::GetIsLineOfSightClear(origin, chest, true, false, false, false, false, true, false);
		aim.assistSightTime = now;
		aim.assistHandle = handle;
	}
	if(!aim.assistVisible)
		return;
	aim.point = chest;
	aim.hasTarget = true;
	aim.engaged = true;
	const CVector toChest = chest - origin;
	if(toChest.Magnitude2D() > kMinAimDistance)
		aim.heading = Atan2(-toChest.x, toChest.y);
}

} // namespace

void
CCoop::Init(void)
{
	// Not the peds: whoever is resetting the game owns those.  CGame clears
	// CWorld::Players before the world is emptied, and the world frees every
	// ped in it.
	ms_bRunning = false;
	s_padHere = false;
	s_padEver = false;
	s_joined = false;
	s_joinTold = false;
	s_padSince = 0;
	s_padLast = 0;
	s_spawnTime = 0;
	s_regroupLogTime = 0;
	s_downTime = 0;
	s_leadPosValid = false;
	s_tetherTold = false;
	s_announceJoin = true;
	s_carry.valid = false;
	s_wantSlot = -1;
	ForgetArsenal();
	s_leadWanted = 0;
	s_partnerWanted = 0;
	s_reviveTime = 0;
	s_reviveValid = false;
	for(int i = 0; i < 2; i++){
		s_aim[i].pointerSeen = false;
		s_aim[i].stickSeen = false;
		s_aim[i].active = false;
		s_aim[i].hasTarget = false;
		s_aim[i].engaged = false;
		s_aim[i].facing = false;
		s_aim[i].drawReticle = false;
		s_aim[i].heading = 0.0f;
		s_aim[i].rawHeading = 0.0f;
		s_aim[i].assistHandle = -1;
	}
}

CPlayerPed *
CCoop::GetPartner(void)
{
	return CWorld::Players[PARTNER].m_pPed;
}

bool
CCoop::PairActive(void)
{
	return ms_bRunning && s_joined;
}

int
CCoop::GetPlayerIndex(const CEntity *entity)
{
	if(entity == nil)
		return -1;
	if(entity == CWorld::Players[LEAD].m_pPed)
		return LEAD;
	if(entity == CWorld::Players[PARTNER].m_pPed)
		return PARTNER;
	return -1;
}

bool
CCoop::FriendlyFireBlocked(CEntity *attacker, CEntity *victim)
{
	if(CoopFriendlyFire || attacker == nil || victim == nil || attacker == victim)
		return false;
	if(!victim->IsPed())
		return false;
	CPed *victimPed = (CPed*)victim;
	if(!victimPed->IsPlayer())
		return false;
	CPed *culprit = attacker->IsPed() ? (CPed*)attacker :
		attacker->IsVehicle() ? ((CVehicle*)attacker)->pDriver : nil;
	return culprit != nil && culprit != victimPed && culprit->IsPlayer();
}

void
CCoop::ReportPartnerPad(int partner, bool present)
{
	// Only the first partner is listened to until the co-op layer grows its own
	// four player loops (stage 3 of the four player work).  The pad layer already
	// reports every partner slot, so that stage is a change of this guard and
	// not of the pads.
	if(partner != 0)
		return;
	const uint32 now = CTimer::GetTimeInMilliseconds();
	if(!present){
		s_padHere = false;
		return;
	}
	if(!s_padHere){
		s_padHere = true;
		s_padSince = now;
	}
	s_padLast = now;
	s_padEver = true;
}

void
CCoop::ReportPointer(int player, float x, float y)
{
	if(player < 0 || player > PARTNER)
		return;
	s_aim[player].pointerX = Clamp(x, 0.0f, 1.0f);
	s_aim[player].pointerY = Clamp(y, 0.0f, 1.0f);
	s_aim[player].pointerTime = CTimer::GetTimeInMilliseconds();
	s_aim[player].pointerSeen = true;
}

void
CCoop::Suspend(const char *why)
{
	RemovePartner(why, true);
	s_spawnTime = CTimer::GetTimeInMilliseconds() + 1500;
}

void
CCoop::Update(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();
	const char *blocker = SessionBlocker();
	const bool allowed = blocker == nil;
	if(allowed != ms_bRunning){
		ms_bRunning = allowed;
		COOP_LOG("WII coop: session %s%s\n", allowed ? "on" : "off: ", allowed ? "" : blocker);
		s_spawnTime = now;
		s_leadPosValid = false;
		// Someone who was playing a moment ago is owed a word about where
		// they went.  Only for a mission: a cutscene explains itself, and the
		// menu toggle was their own doing.  And only into an empty help box:
		// the scripts have already run this frame, and a mission that opened
		// with a hint of its own said something more useful than this.
		if(!allowed && GetPartner() != nil && CTheScripts::IsPlayerOnAMission() &&
		   CHud::m_HelpMessage[0] == 0 && CHud::m_HelpMessageState == 0)
			Tell("WII_CCM");
	}
	if(!ms_bRunning){
		// A partner never outlives the session.  That is what "off during
		// missions" comes to in practice: by the time a mission's first scene
		// starts there is one player ped in the world, as its script assumes.
		if(GetPartner() != nil){
			// Player 1's own wasted or busted resets the pair: the engine clears
			// his weapons at the hospital or the station, and the partner's
			// arsenal is his -- so the partner comes back to whatever player 1
			// has, which after that is nothing.  Every other way a session ends
			// is a pause, not a reset, and the partner keeps what they carry.
			const bool reset = blocker != nil && strcmp(blocker, "player 1 down") == 0;
			RemovePartner(blocker, !reset);
		}
		s_aim[LEAD].active = false;
		s_aim[LEAD].drawReticle = false;
		// A focus the partner chose for a drive does not outlive the session.
		ms_bPartnerFocus = false;
		return;
	}

	CPlayerPed *lead = CWorld::Players[LEAD].m_pPed;
	CPad *leadPad = CPad::GetPad(0);
	CPad *partnerPad = CPad::GetPad(PAD_COOP);

	// Whatever takes player 1's controls away takes the partner's with them,
	// and the two pads read their buttons the same way.
	partnerPad->Mode = leadPad->Mode;
	partnerPad->DisablePlayerControls = leadPad->DisablePlayerControls;

	// Player 1's camera button picks a framing, since the modes it normally
	// steps through are not on offer while the view is shared.
	if(PairActive() && leadPad->CycleCameraModeUpJustDown() && !TheCamera.m_WideScreenOn){
		static const char *const kFramingKeys[NUM_FRAMINGS] = { "WII_CF0", "WII_CF1", "WII_CF2", "WII_CF3" };
		ms_nFraming = (ms_nFraming + 1) % NUM_FRAMINGS;
		if(ms_nFraming < 0)
			ms_nFraming = 0;
		COOP_LOG("WII coop: framing %d\n", (int)ms_nFraming);
		Tell(kFramingKeys[ms_nFraming]);
	}

	// The partner's camera button hands the shared camera's turn to their car.
	// The camera can only follow one nose, and following player 1's leaves a
	// partner in their own car driving half blind whenever player 1 turns.  Only
	// the partner has this button free: player 1's camera button is the framing
	// cycle above.
	if(PairActive() && partnerPad->CycleCameraModeUpJustDown() && !TheCamera.m_WideScreenOn){
		ms_bPartnerFocus = !ms_bPartnerFocus;
		COOP_LOG("WII coop: camera focus %s\n", ms_bPartnerFocus ? "player 2" : "player 1");
		Tell(ms_bPartnerFocus ? "WII_CFP" : "WII_CFL");
	}

	// A script moving player 1 somewhere else -- into a building, to a safe
	// house -- leaves the partner where they were.  Travel never covers this
	// much ground in a frame, on foot or in anything.
	const CVector leadPos = (lead->bInVehicle && lead->m_pMyVehicle) ? lead->m_pMyVehicle->GetPosition() : lead->GetPosition();
	const bool leadTeleported = s_leadPosValid && (leadPos - s_leadPos).Magnitude() > kTeleportStep;
	s_leadPos = leadPos;
	s_leadPosValid = true;

	const bool padPresent = s_padHere && now - s_padSince >= kJoinAfterMs;
	const bool padGone = !s_padHere && (!s_padEver || now - s_padLast > kDropAfterMs);
	if(padGone){
		s_joined = false;
		s_joinTold = false;
		// The camera focus was this partner's choice for their drive.  Their
		// pad is gone, so it goes with them: a partner who joins later gets
		// player 1's car, not a camera that prefers someone else's.
		ms_bPartnerFocus = false;
	}else if(padPresent && !s_joined){
		const CControllerState &held = partnerPad->NewState;
		if(held.Cross || held.Circle || held.Square || held.Triangle ||
		   held.LeftShoulder1 || held.LeftShoulder2 || held.RightShoulder1 || held.RightShoulder2 ||
		   held.DPadUp || held.DPadDown || held.DPadLeft || held.DPadRight)
			s_joined = true;
		else if(!s_joinTold && CHud::m_HelpMessage[0] == 0 && CHud::m_HelpMessageState == 0){
			s_joinTold = true;
			Tell("WII_P2J");
		}
	}

	CPlayerPed *partner = GetPartner();
	if(partner != nil){
		const char *regroup;
		if(padGone){
			RemovePartner("controller gone", false);
			s_spawnTime = now;
			s_announceJoin = true;
			Tell("WII_P2O");
		}else if(partner->DyingOrDead() || partner->m_nPedState == PED_ARRESTED ||
		         (partner->bInVehicle && partner->m_fHealth <= 1.0f)){
			// Nothing in the engine brings a player back except CGameLogic, and
			// CGameLogic only knows the player in focus.  So this is the whole of
			// the partner's wasted-and-busted: down where they fell, and then
			// either brought back by the other player reaching them or, when the
			// bleed-out runs out, back beside player 1 as before.
			//
			// The last test is someone killed in a car.  CPed::InflictDamage does
			// not let them die there: it leaves them one point of health and an
			// order to get out and die on the pavement -- which a passenger cannot
			// carry out while the car is moving, so they would ride along at one
			// health for as long as player 1 kept driving.
			const bool busted = partner->m_nPedState == PED_ARRESTED;
			if(s_downTime == 0){
				s_downTime = now;
				if(!busted)
					Tell("WII_P2D");
			}else if(!busted && SomeoneCanRevive(partner)){
				if(s_reviveTime == 0)
					s_reviveTime = now;
				else if(now - s_reviveTime >= kReviveHoldMs){
					s_reviveTime = 0;
					if(partner->DyingOrDead()){
						if(partner->bInVehicle && partner->m_pMyVehicle != nil){
							// Dead in a vehicle -- a car that drowned, a bike that
							// threw them.  Nowhere to stand them up, so this is the
							// ordinary return, beside player 1.
							RemovePartner("revived", false);
						}else{
							// Dead on the ground: a fresh partner where the body is,
							// through the same machinery as every other arrival, and
							// with the pair's set -- a partner's death is not a reset.
							const CVector at = partner->GetPosition();
							RemovePartner("revived", false);
							s_reviveAt = at;
							s_reviveValid = true;
						}
						s_spawnTime = now;
					}else{
						// Not dead yet: one point of health in a car, waiting to get
						// out and die.  Healed where they sit, and the order to die
						// taken off them.
						partner->m_fHealth = CWorld::Players[LEAD].m_nMaxHealth;
						if(partner->m_objective == OBJECTIVE_LEAVE_CAR_AND_DIE)
							partner->SetObjective(OBJECTIVE_NONE);
						partner->m_leaveCarTimer = 0;
						s_downTime = 0;
					}
					Tell("WII_P2R");
				}
			}else{
				s_reviveTime = 0;
				if(now - s_downTime > kDownedBleedMs){
					RemovePartner(busted ? "busted" : "wasted", false);
					s_spawnTime = now;
				}
			}
		}else if((regroup = NeedsRegroup(lead, partner, leadTeleported)) != nil){
			// A partner who is driving their own car is moved with it.  The
			// leash fires because the world is only streamed around player 1, so
			// the pair has to come back to them -- but the car is the thing that
			// went too far, and deleting the partner out of it throws away the
			// drive and leaves the car abandoned.
			CVehicle *driven = (partner->bInVehicle && partner->m_pMyVehicle != nil &&
			                    partner->m_pMyVehicle->pDriver == partner) ? partner->m_pMyVehicle : nil;
			CVehicle *leadVehicle = (lead->bInVehicle && lead->m_pMyVehicle != nil &&
			                         lead->m_pMyVehicle->pDriver == lead) ? lead->m_pMyVehicle : nil;
			int32 node = -1;
			bool beside = false;
			bool airborne = false;
			CVector airPos;
			bool boat = false;
			CVector boatPos;
			if(driven != nil){
				const CVector leadAt = leadVehicle != nil ? leadVehicle->GetPosition() : lead->GetPosition();
				const float heading = leadVehicle != nil ? leadVehicle->GetForward().Heading() : lead->m_fRotationCur;
				const CVector forward(-Sin(heading), Cos(heading), 0.0f);
				if(driven->IsBoat()){
					// A boat belongs on water, and a road node is not one.  Back
					// on the sea behind player 1 when there is any there -- the
					// direction a boat would have come from -- and ahead of them
					// if not, which is where the water is when they are standing
					// on a pier.  Otherwise it is left where it is: a boat in the
					// road is worse than a boat out of sight.
					for(float back = kRegroupBack; back <= kRegroupBack*3.0f && !boat; back += kRegroupBack){
						const CVector pos = leadAt - forward*back;
						float level;
						if(CWaterLevel::GetWaterLevel(pos, &level, true)){
							boatPos = CVector(pos.x, pos.y, level + 1.0f);
							boat = true;
						}
					}
					for(float ahead = kRegroupBack; ahead <= kRegroupBack*3.0f && !boat; ahead += kRegroupBack){
						const CVector pos = leadAt + forward*ahead;
						float level;
						if(CWaterLevel::GetWaterLevel(pos, &level, true)){
							boatPos = CVector(pos.x, pos.y, level + 1.0f);
							boat = true;
						}
					}
				}else if(driven->IsRealHeli() || driven->IsRealPlane()){
					// An aircraft is not on the road, and a path node would put it
					// in the street: it goes back into the air behind player 1,
					// level with them if they are flying and well above them if
					// they are not.  Anything solid in the way sends it higher.
					airPos = leadAt - forward*kRegroupBack;
					airPos.z = leadAt.z + (leadVehicle != nil &&
						(leadVehicle->IsRealHeli() || leadVehicle->IsRealPlane()) ? 0.0f : 30.0f);
					for(int tries = 0; tries < 4 &&
						CWorld::TestSphereAgainstWorld(airPos, 4.0f, nil, true, true, false, true, false, false) != nil; tries++)
						airPos.z += 15.0f;
					airborne = true;
				}else{
					// On the road behind player 1 rather than on top of them.  The
					// node nearest player 1 is the one their own car is standing on,
					// and two cars put in the same square metre spend the next few
					// seconds pushing each other out of it -- the regroup that fires
					// every second.  Behind is also the direction the partner can
					// drive on in, which is where they were going anyway.
					for(float back = kRegroupBack; back <= kRegroupBack*3.0f && node < 0; back += kRegroupBack){
						const int32 candidate = ThePaths.FindNodeClosestToCoors(leadAt - forward*back, PATH_CAR, kRegroupSearch);
						if(candidate < 0)
							continue;
						if((ThePaths.m_pathNodes[candidate].GetPosition() - leadAt).Magnitude2D() < kRegroupClearance)
							continue;
						node = candidate;
					}
					// Nothing clear behind: the node nearest player 1 after all, so
					// the car is at least on something.  It goes to the side of them
					// if that node is the one they are on.
					if(node < 0){
						node = ThePaths.FindNodeClosestToCoors(leadAt, PATH_CAR, 100.0f);
						beside = node >= 0 && leadVehicle != nil &&
						         (ThePaths.m_pathNodes[node].GetPosition() - leadAt).Magnitude2D() < kRegroupClearance;
					}
				}
			}
			if(boat){
				driven->SetPosition(boatPos);
				if(leadVehicle != nil){
					driven->SetOrientation(0.0f, 0.0f, leadVehicle->GetForward().Heading());
					// A boat only keeps pace with another boat; a car's speed is
					// not a speed this one can hold.
					driven->m_vecMoveSpeed = leadVehicle->IsBoat() ? leadVehicle->GetMoveSpeed() : CVector(0.0f, 0.0f, 0.0f);
				}else
					driven->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
				driven->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
				if(now - s_regroupLogTime >= 3000){
					s_regroupLogTime = now;
					COOP_LOG("WII coop: player 2's boat brought back (%s)\n", regroup);
				}
			}else if(airborne){
				driven->SetPosition(airPos);
				if(leadVehicle != nil)
					driven->SetOrientation(0.0f, 0.0f, leadVehicle->GetForward().Heading());
				driven->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
				driven->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
				if(now - s_regroupLogTime >= 3000){
					s_regroupLogTime = now;
					COOP_LOG("WII coop: player 2's aircraft brought back (%s)\n", regroup);
				}
			}else if(node >= 0){
				CVector pos = ThePaths.m_pathNodes[node].GetPosition();
				if(beside)
					pos += leadVehicle->GetRight()*kRegroupClearance;
				pos.z += 1.0f;
				driven->SetPosition(pos);
				// Facing the way player 1 faces, so taking the wheel back does
				// not begin with a U-turn.
				if(leadVehicle != nil)
					driven->SetOrientation(0.0f, 0.0f, leadVehicle->GetForward().Heading());
				// Moving with player 1 rather than parked in their road.  A car
				// put down at rest fifty metres behind a car doing fifty is
				// fifty metres behind again a second later, and the regroup
				// fires again -- the loop this is here to stop.
				if(leadVehicle != nil)
					driven->m_vecMoveSpeed = leadVehicle->GetMoveSpeed();
				else
					driven->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
				driven->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
				if(now - s_regroupLogTime >= 3000){
					s_regroupLogTime = now;
					COOP_LOG("WII coop: player 2's car brought back (%s)\n", regroup);
				}
			}else if(driven != nil && driven->IsBoat()){
				// Nowhere to put it: left alone rather than deleted.  The world
				// around it is thin, but a boat respawned on foot is worse.
				if(now - s_regroupLogTime >= 3000){
					s_regroupLogTime = now;
					COOP_LOG("WII coop: player 2's boat left where it is (no water by player 1)\n");
				}
			}else{
				// Replaced rather than moved.  Moving a ped that might be halfway
				// through a door, a fall or a punch means unpicking whichever of
				// those it is; a fresh one beside player 1 is in a known state,
				// and carries over the health and the weapon the old one had.
				RemovePartner(regroup, true);
				s_spawnTime = now;
			}
		}else{
			s_downTime = 0;
			// These are set on every passenger of a car whose driver is dragged
			// out, with no exception for players, and would send the partner
			// running from the car under the engine's control.
			partner->bFleeAfterExitingCar = false;
			partner->bHeldHostageInCar = false;
			partner->m_area = lead->m_area;
			// Player 1's car is entered as a passenger or not at all.  The
			// partner is sent to an empty seat, but if somebody takes it while
			// they walk over, CPed::SeekCar has them drag that somebody out
			// instead -- and CPed::PedSetInCarCB then makes the partner the
			// car's driver, over the top of player 1, who is sitting in it.
			// Stopped here, on the frame it starts, before anyone is pulled
			// anywhere.
			//
			// A car the partner is taking as its driver is meant to be a
			// carjack, occupied or not -- cancelling every PED_CARJACK is what
			// stopped the partner stealing an occupied car at all -- but never
			// with a player in the driver's seat: that is not a carjack, it is
			// one player pulling the other out of their own car.
			if(partner->m_nPedState == PED_CARJACK &&
			   (partner->m_objective == OBJECTIVE_ENTER_CAR_AS_PASSENGER ||
			    (partner->m_carInObjective != nil && partner->m_carInObjective->pDriver != nil &&
			     partner->m_carInObjective->pDriver->IsPlayer())))
				partner->QuitEnteringCar();
			if(UpdatePartnerVehicle(lead, partner, partnerPad)){
				// Replaced, like every other time the partner is moved; the new
				// one is made in the seat.  SpawnPartner runs just below.
				RemovePartner("hopping in", true);
				s_spawnTime = now;
			}else
				UpdateArsenal(lead, partner);
		}
		partner = GetPartner();
		if(partner != nil){
			SyncSharedAmmo(lead, partner);
			ShareWantedLevel(lead, partner);
		}
	}
	if(partner == nil && padPresent && s_joined && now >= s_spawnTime){
		if(!SpawnPartner(lead))
			s_spawnTime = now + kRetryMs;
	}

	UpdateAim(LEAD);
	UpdateAim(PARTNER);
}

void
CCoop::LimitSeparation(CPed *ped, CVector2D &moved)
{
	if(!ms_bRunning)
		return;
	const int index = GetPlayerIndex(ped);
	if(index < 0)
		return;
	// Only between two players who are both up and both on foot.  Someone in a
	// car is not held by this -- the leash deals with them -- and someone lying
	// dead in the road does not get to keep the other one standing over them.
	CPlayerPed *other = CWorld::Players[index == LEAD ? PARTNER : LEAD].m_pPed;
	if(other == nil || other->bInVehicle || other->DyingOrDead() || other->m_nPedState == PED_ARRESTED)
		return;

	CVector2D apart(ped->GetPosition().x - other->GetPosition().x, ped->GetPosition().y - other->GetPosition().y);
	const float distance = apart.Magnitude();
	if(distance <= kTetherStart)
		return;
	apart = apart*(1.0f/distance);
	const float outward = moved.x*apart.x + moved.y*apart.y;
	if(outward <= 0.0f)
		return;

	// All of the outward speed at the start of the band, none of it at the end,
	// so the wall is something a player runs out of rather than into.
	const float allowed = Clamp((kTetherMax - distance)/(kTetherMax - kTetherStart), 0.0f, 1.0f);
	moved.x -= apart.x*outward*(1.0f - allowed);
	moved.y -= apart.y*outward*(1.0f - allowed);

	if(allowed < 0.2f && !s_tetherTold){
		s_tetherTold = true;
		Tell("WII_TTH");
	}
}

bool
CCoop::UsesReticleAim(void)
{
	// The mode test is what makes this exact.  The shared camera stands aside
	// for garages, for the arrest and death cameras and for anything a script
	// points the view at, and in all of those the players are back on the
	// controls the stock game gives them.
	return ms_bRunning && CCamera::m_bUseMouse3rdPerson &&
		TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_WII_COOP;
}

bool
CCoop::FacesAim(CPlayerPed *ped, CPad *pad)
{
	const int index = GetPlayerIndex(ped);
	if(index < 0)
		return false;
	CoopAim &aim = s_aim[index];
	aim.facing = false;
	if(!UsesReticleAim() || !aim.active)
		return false;

	// Running flat out is travelling, not fighting: the engine will not let a
	// sprinting player fire anyway.  Facing the reticle then would mean running
	// sideways or backwards most of the time, at the pace those animations move.
	if(pad->GetSprint() && (pad->GetPedWalkLeftRight() != 0 || pad->GetPedWalkUpDown() != 0))
		return false;

	// With a gun out, where you point is where you look.
	//
	// With fists, a bat or a blade it is where you are going, and it stays so
	// through the swing: the engine's own fight picks who gets hit and turns
	// the body onto them for each blow, and a second opinion from the reticle
	// pulls every one of them off its mark.  Except crouched, and only because
	// the Classic control this falls back to does not let a crouching player
	// move at all.
	if(CWeaponInfo::GetWeaponInfo(ped->GetWeapon()->m_eWeaponType)->m_eWeaponFire == WEAPON_FIRE_MELEE)
		aim.facing = ped->bIsDucking && ped->m_nPedState != PED_FIGHT;
	else
		aim.facing = true;
	return aim.facing;
}

bool
CCoop::IsFacingAim(CPed *ped)
{
	const int index = GetPlayerIndex(ped);
	return index >= 0 && s_aim[index].facing && s_aim[index].active;
}

bool
CCoop::HasAim(CPed *ped)
{
	const int index = GetPlayerIndex(ped);
	return index >= 0 && s_aim[index].active;
}

float
CCoop::GetAimHeading(CPed *ped)
{
	const int index = GetPlayerIndex(ped);
	if(index >= 0 && s_aim[index].active)
		return s_aim[index].heading;
	return ped->m_fRotationCur;
}

float
CCoop::GetAimPitch(CPed *ped)
{
	const int index = GetPlayerIndex(ped);
	if(index < 0 || !s_aim[index].active || !s_aim[index].hasTarget)
		return 0.0f;
	// From about where the gun is held.
	CVector from = ped->GetPosition();
	from.z += 0.4f;
	const CVector to = s_aim[index].point - from;
	return -Atan2(to.z, Max(to.Magnitude2D(), 0.1f));
}

bool
CCoop::FindShotVector(CEntity *shooter, float range, const CVector &fireSource, CVector &source, CVector &target)
{
	source = fireSource;
	const int index = GetPlayerIndex(shooter);
	float heading = shooter->GetForward().Heading();
	// The reticle, if the gun is pointing anywhere near it (kAimArc); the gun's
	// own line if not.
	if(index >= 0 && s_aim[index].active &&
	   Abs(CGeneral::LimitRadianAngle(s_aim[index].heading - heading)) <= kAimArc){
		const CoopAim &aim = s_aim[index];
		if(aim.hasTarget){
			CVector dir = aim.point - fireSource;
			const float length = dir.Magnitude();
			if(length > 0.3f){
				target = fireSource + dir*(range/length);
				return true;
			}
		}
		heading = aim.heading;
	}
	// A direction and nothing more: fire level along it.  From a muzzle held at
	// chest height that is the line a top-down player expects -- it carries to
	// the weapon's full range instead of ending in the ground at the reticle.
	target = fireSource + CVector(-Sin(heading), Cos(heading), 0.0f)*range;
	return false;
}

bool
CCoop::GetReticle(int player, float &x, float &y, bool &engaged)
{
	if(player < 0 || player > PARTNER || !s_aim[player].drawReticle)
		return false;
	x = s_aim[player].reticleX;
	y = s_aim[player].reticleY;
	engaged = s_aim[player].engaged;
	return true;
}
