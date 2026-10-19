#include "common.h"

#include "Coop.h"
#include "CoopModes.h"
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
#include "References.h"
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
#include "WiiPad.h"
#include "WiiTrace.h"
#endif

// Transitions only.  Co-op cannot be run on anything but a console with
// several people in front of it, so when it misbehaves the log is the whole of
// the evidence -- but a session that is working writes a handful of these lines
// and no more, which keeps it inside the "signal, not trace" budget the rest of
// the port's logging holds to.
#ifdef NINTENDO_WII
#define COOP_LOG(...) WiiTraceReport(__VA_ARGS__)
#else
#define COOP_LOG(...) debug(__VA_ARGS__)
#endif

bool CCoop::ms_bRunning;
int8 CCoop::ms_nFraming = 1;
int CCoop::ms_nCameraFocus = -1;
int CCoop::ms_nAutoJoinPartners;

// The co-op options that are not pad settings, set from the co-op page and
// saved in the INI under "Wii".  Friendly fire is on by default -- the game as
// it was, with no rule against players shooting each other.  The wanted level
// is not an option: the party always shares one, because the police only ever
// read player 1's; see ShareWantedLevel.
//
// The minigames are on by default too: they only ever arm from something the
// party does on purpose -- a siren, in the cop shift's case -- so the switch
// is there for a party that wants the city to itself.
int8_t CoopFriendlyFire = 1;
int8_t CoopMinigames = 1;

namespace
{

enum
{
	// Player slots in CWorld::Players: 0 is player 1, 1..NUMPLAYERS-1 are the
	// partners.  A partner's pad is PAD_COOP + (slot - 1); the pad layer
	// numbers partners 0..kNumPartners-1, which is its "partner index".
	LEAD = 0,
	kNumPartners = NUMPLAYERS - 1,
};

// --- when a partner comes and goes ------------------------------------------
// A controller being connected is not a player.  A remote is connected
// whenever it is switched on, and the one that started the game is still
// switched on while its owner plays on a GameCube pad: taking that for a
// partner would stand an idle Tommy beside everyone who plays that way.  So a
// partner arrives when somebody presses a button on their controller.
//
// It has to have been there this long before a button counts.  A Wiimote
// reports its Nunchuk a few frames after it reports itself, and until then it
// is not yet the controller its player is holding.
const uint32 kJoinAfterMs = 400;
// And gone this long before they leave.  Long, because a remote drops out for a
// second or two whenever its batteries sag or someone sits on it, and losing a
// partner to that would read as the game throwing them out.
const uint32 kDropAfterMs = 6000;
// How long a downed partner is left where they fell before coming back beside
// player 1.  Long enough for another player to reach them -- which is what
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
// the party is kept within reach of player 1, in two ways.
//
// On foot it is a wall.  Past kTetherStart a partner walking away from player
// 1 is slowed, and at kTetherMax they are stopped; player 1 is held to each
// partner the same way, so no one can leave the others behind and everyone
// has to mean to go somewhere to get there.  See CCoop::LimitSeparation.  A
// partner who just stands there while player 1 pushes against the wall is
// brought back too, so nobody can be pinned for good; see kWallHoldMs.
//
// The wall only works on someone walking.  A car driving off, a fall, the
// blast from an explosion all get past it, and for those there is kLeash: the
// partner is brought back to player 1 -- into a passenger seat, if there is
// one going.
//
// kCoopMaxDistance in Cam.cpp is sized to keep two players on screen out to
// the wall, in the direction the camera sees least far; with more players the
// tether is the lever, not the zoom (see COOP-4-PLAN.md).
const float kTetherStart = 18.0f;
const float kTetherMax = 24.0f;
const float kLeash = 28.0f;
// And the longer one anyone in a vehicle a player is driving gets, the driver
// and their passengers.  A car covers ground far faster than a walk, and a
// convoy is useless if the one behind is hauled forward every second -- at 28 m
// that is about a second at speed.  Still bounded, because the leash exists for
// the streaming and not for taste; this is the number to move if the ground
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
// A node farther from player 1 than the driving leash less this is no place to
// put a car back: the leash would catch it again on the very next frame, and
// the one after, with the car never getting anywhere.  There the partner is
// replaced beside player 1 instead, as for anyone else.
const float kRegroupMargin = 10.0f;
// The wall holds player 1 to every partner on foot, so a partner who is not
// moving -- away from the pad, or stuck behind something -- pins them at the
// edge with nothing but a car to get out of it.  Pushing against one partner's
// end of the wall for this long (kWallHoldMs), with no gap in the pushing
// longer than kWallGapMs and the partner staying within kWallMoved of where they
// stood when it began, brings that partner back beside player 1.  A partner who
// is walking after them is moving and so never counts; a long hold would let a
// stuck partner spoil a long walk, a short one would catch someone who is only
// slow, and both want a feel on hardware.
const uint32 kWallHoldMs = 2500;
const uint32 kWallGapMs = 300;
const float kWallMoved = 3.0f;
// Below player 1 by this much means they fell through something.
const float kFallLimit = 25.0f;
// Player 1 moving further than this in one frame was a teleport, not travel.
const float kTeleportStep = 12.0f;
// A vehicle slower than this (world units per game step, about 5 m/s) is
// near enough to stopped for someone to be put down beside it.
const float kSlowVehicle = 0.1f;
// How close a partner has to be to get into another player's car by pressing
// the button, rather than being expected to walk to it first.
const float kBoardingRange = 12.0f;
// How close a car has to be for a partner to take it as its driver when there
// is no seat with another player.  Matches the box CPlayerInfo::Process scans
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
CoopAim s_aim[NUMPLAYERS];

// The partners' controllers, one row per partner: whether it is there now,
// since when, and when it was last there.  Indexed the way the pad layer
// reports them (0 is the first partner); the player slot is index + 1.
bool s_padHere[kNumPartners];
uint32 s_padSince[kNumPartners];
uint32 s_padLast[kNumPartners];
bool s_padEver[kNumPartners];
// Whether somebody has pressed a button on it since it turned up, and whether
// they have been told that is what it takes.
bool s_joined[kNumPartners];
bool s_joinTold[kNumPartners];
// And when they were last told, so a controller that is pressed without joining
// -- +, - or the stick, none of which is the join button -- is told again, but
// not every time a thumb brushes it.  Also whether the batteries of a joined
// remote have been called low yet, once per connection.
const uint32 kJoinHintMs = 20000;
uint32 s_joinHintAt[kNumPartners];
bool s_batteryTold[kNumPartners];
// A controller that has not been seen this long counts as lost, for the HUD.
// Longer than the pad layer's own grace for a dropped report (twenty scans, a
// third of a second), so a Nunchuk jogged in its socket does not flash a "?".
const uint32 kSilentMs = 1500;
// When each partner may next be put into the world.
uint32 s_spawnTime[NUMPLAYERS];
// When the regroup line was last written.  It is a signal, not a tick: a car
// being yanked every second must not write it every second too.
uint32 s_regroupLogTime;
// When a partner was first seen down -- dead, or out of health in a car with
// an order to die on the pavement -- or 0.  Arrested counts too: that is the
// one of the three nobody can be brought back from.  Per partner.
uint32 s_downTime[NUMPLAYERS];
// Since when another player has been close enough to revive them, or 0.  A
// revive needs the hold, so walking past a body does not do it.
uint32 s_reviveTime[NUMPLAYERS];
// Where a revived partner is put back: where they fell, not beside player 1.
// Consumed by the next SpawnPartner, and dropped if the session goes down
// first.
CVector s_reviveAt[NUMPLAYERS];
bool s_reviveValid[NUMPLAYERS];
// Where player 1 was last frame, to notice a teleport.
CVector s_leadPos;
bool s_leadPosValid;
// Whether the players have been told about the wall yet, and when they last
// were.  They are told again if someone is still leaning on it kTetherRetellMs
// later: the first line goes by while everyone is looking at the other player.
const uint32 kTetherRetellMs = 30000;
bool s_tetherTold;
uint32 s_tetherAt;
// Player 1 held at the wall by a partner on foot, one row per partner: since
// when they have been pushing against it, the last time they were, and where
// the partner stood when it began.  LimitSeparation writes them -- it is the
// one place that knows -- and Update reads them, since it is the one place a
// partner may be replaced.
uint32 s_wallSince[NUMPLAYERS];
uint32 s_wallLast[NUMPLAYERS];
CVector s_wallAt[NUMPLAYERS];
// Since when a partner has been waiting for their skin to stream in, or 0.  A
// skin that has not come in this long is given up on: they arrive as the
// ordinary player model rather than not at all.
const uint32 kSkinWaitMs = 6000;
uint32 s_skinWaitSince[NUMPLAYERS];
// And the last time they were tried for, so a wait that was dropped half-way --
// the controller went, the session stood down -- does not count against the next.
uint32 s_skinWaitLast[NUMPLAYERS];
// Whether a partner's next arrival is news.  Coming back from the dead or
// from the far end of the street is not; a controller being picked up is.
bool s_announceJoin[NUMPLAYERS];
// And whether it is a return that is owed a line: brought back after being
// replaced beside player 1, or after the bleed-out or the arrest that nobody
// reached them from.  Coming back after a mission or a cutscene is not.
bool s_announceReturn[NUMPLAYERS];
// A partner who cannot be put anywhere yet -- player 1 is in a boat or an
// aircraft, or in a moving car with no seat -- is simply not in the world, which
// from the sofa is a partner who has vanished.  Since when they have been
// waiting, the last time they were tried for (the same trick as the skin wait,
// so a wait that was dropped half-way does not count towards the next), and
// whether they have been told.  The line waits for a moment of real waiting
// first, because the car doors opening and closing is not.
const uint32 kWaitNoticeMs = 2000;
uint32 s_waitSince[NUMPLAYERS];
uint32 s_waitLast[NUMPLAYERS];
bool s_waitTold[NUMPLAYERS];
// What the first partner to arrive is told about how the party works.  Once
// per run of the console, not once per game: a player who loads a save has
// read it.
bool s_introTold;
// The party is held for a mission, and whether the help box has had its line
// about it yet.  The line is told at the first moment the screen can show it,
// not when the mission starts, because the mission's own cutscene is usually on
// screen then and takes the help box away with it.
const uint32 kMissionRetellMs = 20000;
bool s_missionHeld;
bool s_missionTold;
uint32 s_missionTellAt;

// --- what the help box is told ------------------------------------------------
// A line in the help box, for the few things that happen with nothing on screen
// to show for them: a controller being noticed, a partner going because theirs
// went, the framing changing.  The words are in Text.cpp's fallback table,
// since the user's own GXT files are never rewritten.
//
// There is one help box and a join, a revive and a framing can all land in the
// same second, so a notice waits its turn: Tell queues the key and
// ShowNextNotice, once a frame from CCoop::Update, puts the next one up when the
// box has finished with whatever was there -- one of the game's own hints, or
// the last of these.  A handful of keys, pointing at strings that live for the
// whole run, so nothing is allocated.  A notice already waiting is not queued
// twice, and one that comes when the queue is full pushes out the oldest, which
// is the stalest news.  News that has waited its stale limit for the box -- one
// of the game's own hints that stays up, say -- is dropped rather than shown
// late: "paused for the mission" is no use once the mission is over.
//
// A line is shown for the help box's ordinary time, three seconds and a twentieth
// of one per character, so that it can be read; the camera notices alone are
// quick (see ShowNextNotice).  That makes the queue slow -- five or six seconds
// a line with the fades -- which is why the numbers are what they are.  Three
// lines can be waiting at once, a partner's arrival and the two that explain the
// party, and the third should still be news when its turn comes: the stale limit
// is long enough for two lines ahead of it, and the queue deep enough that a burst
// of arrivals does not push the explanations out.  The lines that are only true
// for a moment get a shorter limit of their own, and the explanations a longer
// one, since they are as true in half a minute as now.
const int kMaxNotices = 8;
const uint32 kNoticeStaleMs = 15000;
const uint32 kNoticeStaleShortMs = 8000;
const uint32 kNoticeStaleLongMs = 60000;
const char *s_notice[kMaxNotices];
uint32 s_noticeAt[kMaxNotices];
uint32 s_noticeStale[kMaxNotices];
int s_noticeCount;

// Whether the help box can take a new line without wiping one.  Nothing on
// screen is state 0.  SetHelpMessage also leaves the box in state 0 until the
// HUD next draws it, with the line set and the copy the HUD last drew cleared,
// so a line that is set and not yet drawn -- one of ours from a moment ago, or a
// script's from this frame -- counts as being in use too.
bool
HelpBoxFree(void)
{
	return CHud::m_HelpMessageState == 0 &&
		!(CHud::m_HelpMessage[0] != 0 && CHud::m_LastHelpMessage[0] == 0);
}

// Whether two notices say the same thing.  Camera notices are one topic: a
// player tapping the camera button through the framings wants the framing they
// have now, not each of the ones they went through, one after another.
bool
SameNotice(const char *a, const char *b)
{
	return strcmp(a, b) == 0 || (strncmp(a, "WII_CF", 6) == 0 && strncmp(b, "WII_CF", 6) == 0);
}

// Takes the oldest notice off the queue.
void
DropOldestNotice(void)
{
	for(int i = 1; i < s_noticeCount; i++){
		s_notice[i-1] = s_notice[i];
		s_noticeAt[i-1] = s_noticeAt[i];
		s_noticeStale[i-1] = s_noticeStale[i];
	}
	s_noticeCount--;
}

void
Tell(const char *key, uint32 staleMs = kNoticeStaleMs)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();
	for(int i = 0; i < s_noticeCount; i++){
		if(SameNotice(s_notice[i], key)){
			s_notice[i] = key;
			s_noticeAt[i] = now;
			s_noticeStale[i] = staleMs;
			return;
		}
	}
	if(s_noticeCount == kMaxNotices)
		DropOldestNotice();
	s_notice[s_noticeCount] = key;
	s_noticeAt[s_noticeCount] = now;
	s_noticeStale[s_noticeCount] = staleMs;
	s_noticeCount++;
}

void
ShowNextNotice(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();
	while(s_noticeCount > 0 && now - s_noticeAt[0] > s_noticeStale[0])
		DropOldestNotice();
	if(s_noticeCount == 0 || !HelpBoxFree())
		return;
	// Only the camera's lines are quick, a second and a half on the screen: they
	// answer a button press the player is looking at the screen for, and pressing
	// it again replaces them.  Everything else is news the party has to be able to
	// read, in the time the game gives its own hints.
	CHud::SetHelpMessage(TheText.Get(s_notice[0]), strncmp(s_notice[0], "WII_CF", 6) == 0);
	DropOldestNotice();
}

// The few things that happen to one player with nothing on screen to show for
// them -- brought back to the group, held for a seat -- say so at most once per
// kSituationMs for that player, so a car that is hauled back every few seconds
// does not turn the help box into a ticker.  Returns whether it was told.
const uint32 kSituationMs = 10000;
uint32 s_situationAt[NUMPLAYERS];

bool
TellAbout(int player, const char *key)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();
	if(s_situationAt[player] != 0 && now - s_situationAt[player] < kSituationMs)
		return false;
	s_situationAt[player] = now;
	Tell(key);
	return true;
}

// One help line per partner, so a line about player 3 says player 3.  The
// words are in Text.cpp's fallback table, since the user's own GXT files are
// never rewritten.
const char *const kJoinKeys[kNumPartners] = { "WII_P2J", "WII_P3J", "WII_P4J" };
const char *const kJoinedKeys[kNumPartners] = { "WII_P2I", "WII_P3I", "WII_P4I" };
const char *const kLeftKeys[kNumPartners] = { "WII_P2O", "WII_P3O", "WII_P4O" };
const char *const kDownKeys[kNumPartners] = { "WII_P2D", "WII_P3D", "WII_P4D" };
const char *const kBackKeys[kNumPartners] = { "WII_P2R", "WII_P3R", "WII_P4R" };
// Brought back to the party, waiting for a seat, waiting out a boat or aircraft
// ride, and a remote whose batteries are low.
const char *const kGroupKeys[kNumPartners] = { "WII_P2G", "WII_P3G", "WII_P4G" };
const char *const kSeatKeys[kNumPartners] = { "WII_P2S", "WII_P3S", "WII_P4S" };
const char *const kRideKeys[kNumPartners] = { "WII_P2B", "WII_P3B", "WII_P4B" };
const char *const kBatteryKeys[kNumPartners] = { "WII_P2L", "WII_P3L", "WII_P4L" };

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
Carry s_carry[NUMPLAYERS];
// The weapon slot each partner should end up holding once it arrives.
int8 s_wantSlot[NUMPLAYERS];
uint32 s_wantSlotUntil[NUMPLAYERS];

// --- the partners' weapons ---------------------------------------------------
// Pickups, shops and scripts all hand weapons to FindPlayerPed(), and that is
// left exactly as it is.  Every partner is given the same WEAPONS as player 1
// -- one pickup arms the party -- but they all draw from ONE AMMO POOL, not a
// copy each.
//
// The pool is player 1's m_nAmmoTotal and each partner's is kept equal to it.
// The clip and the reload timer stay each player's own: they are what makes a
// reload take time, and sharing the clip too would let several magazines of
// rounds exist against one count.  When a partner fires, their clip drops as
// usual and the rounds they used are taken out of the pool at the next
// CCoop::Update (SyncSharedAmmo).  So a pickup arms the party, a burst from
// one empties the others, and nobody gets a second copy of the same rounds.
//
// This is why a partner's own total is never trusted: it is only a cache of
// the pool, re-synced every frame.  A magazine is clamped to what the pool has
// left, so it cannot hold rounds the others have already spent.
//
// What has been seen of player 1's weapons is kept for as long as the game is,
// not for as long as the partners are.  A partner who is only being moved
// takes their own weapons with them (Carry); one who is new, or who died,
// starts from a copy of everything player 1 has.
eWeaponType s_seenType[TOTAL_WEAPON_SLOTS];
// Owed to each partner but not yet handed over, because its model is still
// streaming in.  Given with no ammo: the pool is the ammo.  One row per
// player slot; a partner's row survives their being moved (Carry) and is
// cleared when they are replaced from nothing.
eWeaponType s_owedType[NUMPLAYERS][TOTAL_WEAPON_SLOTS];
// The shared pool as of the last sync, per partner per slot.  -1 means this
// slot is not shared yet for that partner (nothing there, or a weapon they
// have not been given).
int32 s_poolAmmo[NUMPLAYERS][TOTAL_WEAPON_SLOTS];

// --- the wanted level ---------------------------------------------------------
// One heat for the party, always; there is no option to split it.  A crime by
// anyone is the party's crime, so when any player's level rises everyone goes
// to the highest of them -- and the stars on the HUD, player 1's, are the ones
// the party is judged by, because every cop reads player 1's level and nobody
// else's.  Most crimes are registered on player 1's record to begin with; the
// rise is what carries across the few that land on a partner's own, such as a
// cop seeing one of them point a gun (CPed::ReactToPointGun).  A player who
// joins starts at player 1's level.  Holding everyone at the highest level
// every frame would keep the party wanted for good, so a drop is not mirrored
// as it happens; but one on player 1, the level the police read, is followed.
// CWanted::Update only wears down a single star, so a level above that goes
// away because something cleared it -- a Pay'n'Spray, a bribe, a cheat -- and a
// partner left at the old level would put the whole party back on it with the
// next small crime, and spend a FindNewAttackPoints scan every frame in the
// meantime.  A partner's own level can be lower than player 1's (a single star
// wears off by itself), and then nobody reads it; the sync never leaves it
// higher.
//
// The one cost is a statistic: SetWantedLevel is also what counts
// CStats::WantedStarsAttained and WantedStarsEvaded, so the party's crimes and
// evasions count those once per player.  Nothing reads them but the stats
// screen.
int32 s_wanted[NUMPLAYERS];

void
ShareWantedLevel(void)
{
	CPlayerPed *lead = CWorld::Players[LEAD].m_pPed;
	if(lead == nil || lead->m_pWanted == nil)
		return;
	const int32 leadLevel = lead->m_pWanted->GetWantedLevel();
	int32 highest = leadLevel;
	bool rose = leadLevel > s_wanted[LEAD];
	const bool leadFell = leadLevel < s_wanted[LEAD];
	for(int i = 1; i < NUMPLAYERS; i++){
		CPlayerPed *partner = CWorld::Players[i].m_pPed;
		if(partner == nil || partner->m_pWanted == nil)
			continue;
		const int32 level = partner->m_pWanted->GetWantedLevel();
		if(level > highest)
			highest = level;
		if(level > s_wanted[i])
			rose = true;
	}
	if(rose){
		for(int i = 0; i < NUMPLAYERS; i++){
			CPlayerPed *ped = CWorld::Players[i].m_pPed;
			if(ped != nil && ped->m_pWanted != nil && ped->m_pWanted->GetWantedLevel() != highest)
				ped->m_pWanted->SetWantedLevel(highest);
		}
	}else if(leadFell){
		// Player 1's level came down with nobody's going up: the party's
		// heat was cleared where the police read it.  Partners above it
		// come down to it, never past it, and one already lower is left.
		for(int i = 1; i < NUMPLAYERS; i++){
			CPlayerPed *partner = CWorld::Players[i].m_pPed;
			if(partner != nil && partner->m_pWanted != nil && partner->m_pWanted->GetWantedLevel() > leadLevel)
				partner->m_pWanted->SetWantedLevel(leadLevel);
		}
	}
	for(int i = 0; i < NUMPLAYERS; i++){
		CPlayerPed *ped = CWorld::Players[i].m_pPed;
		if(ped != nil && ped->m_pWanted != nil)
			s_wanted[i] = ped->m_pWanted->GetWantedLevel();
	}
}

// Forget one partner's synced pool without forgetting the weapons.  For a
// partner that is being replaced: the new ped's counts are theirs to start,
// and the first sync takes them from the pool rather than measuring a spend
// against a stale figure.
void
ForgetSharedAmmo(int player)
{
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++)
		s_poolAmmo[player][slot] = -1;
}

void
ForgetArsenal(void)
{
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++)
		s_seenType[slot] = WEAPONTYPE_UNARMED;
	for(int i = 0; i < NUMPLAYERS; i++){
		for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++)
			s_owedType[i][slot] = WEAPONTYPE_UNARMED;
		ForgetSharedAmmo(i);
	}
}

bool
MirrorsWeapon(eWeaponType type)
{
	// The detonator is handed out by the throw that needs it (CWeapon::Fire), and
	// the camera is a mission prop.
	return type != WEAPONTYPE_UNARMED && type != WEAPONTYPE_DETONATOR && type != WEAPONTYPE_CAMERA;
}

// Forget what one partner is owed and how their ammo counted, without
// touching anybody else: for a partner being replaced from nothing.
void
ClearArsenalFor(int player)
{
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++)
		s_owedType[player][slot] = WEAPONTYPE_UNARMED;
	ForgetSharedAmmo(player);
}

// Queue player 1's whole arsenal for one partner: how a partner who has just
// arrived -- or just been replaced from nothing -- gets everything at once.
// Handed over empty; the ammo is the shared pool.
void
QueueArsenalFor(int player)
{
	CPlayerPed *lead = CWorld::Players[LEAD].m_pPed;
	if(lead == nil)
		return;
	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		const eWeaponType type = lead->GetWeapon(slot).m_eWeaponType;
		if(MirrorsWeapon(type))
			s_owedType[player][slot] = type;
	}
}

void
UpdateArsenal(CPlayerPed *lead)
{
	// A rampage puts its own weapon into one of player 1's slots and takes it
	// out again when it ends.  That slot is not looked at while it lasts:
	// copying it would leave the partners holding a minigun with thirty
	// thousand rounds once it was over, and noting it would make player 1's
	// own weapon coming back afterwards look like a new one.
	const int frenzySlot = CDarkel::GetFrenzyWeaponSlot();

	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		const eWeaponType type = lead->GetWeapon(slot).m_eWeaponType;
		if(slot != frenzySlot){
			if(MirrorsWeapon(type) && type != s_seenType[slot]){
				// New to player 1, so owed to every partner -- including one
				// who is out of the world for a moment, so whoever replaces
				// them still gets it.  The drain below skips a partner who
				// already holds it.
				for(int i = 1; i < NUMPLAYERS; i++)
					s_owedType[i][slot] = type;
			}
			s_seenType[slot] = type;
		}
	}

	for(int i = 1; i < NUMPLAYERS; i++){
		CPlayerPed *partner = CWorld::Players[i].m_pPed;
		if(partner == nil)
			continue;
		for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
			if(s_owedType[i][slot] == WEAPONTYPE_UNARMED)
				continue;
			// The same wait CPed::RequestDelayedWeapon does, and for the same
			// reason: a weapon given before its model has loaded is a weapon
			// with nothing to draw.  Player 1's current gun is always in
			// memory; the rest of what they carry may not be.
			const CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(s_owedType[i][slot]);
			const int32 model1 = info->m_nModelId;
			const int32 model2 = info->m_nModel2Id;
			if(model1 != -1)
				CStreaming::RequestModel(model1, STREAMFLAGS_DEPENDENCY);
			if(model2 != -1)
				CStreaming::RequestModel(model2, STREAMFLAGS_DEPENDENCY);
			if((model1 == -1 || CStreaming::HasModelLoaded(model1)) &&
			   (model2 == -1 || CStreaming::HasModelLoaded(model2))){
				if(partner->GetWeapon(slot).m_eWeaponType == s_owedType[i][slot]){
					// Queued while they were out of the world and they already
					// hold it -- a weapon player 1 lost and picked up again.
					// Nothing to hand over.
					s_owedType[i][slot] = WEAPONTYPE_UNARMED;
				}else{
					partner->GiveWeapon(s_owedType[i][slot], 0, true);
					s_owedType[i][slot] = WEAPONTYPE_UNARMED;
					// A different weapon in the slot is a different pool for it.
					s_poolAmmo[i][slot] = -1;
				}
			}
		}

		// Draw the weapon they were holding, or failing that the one player 1
		// is, as soon as it has arrived.  Asked for the way a pickup asks: by
		// naming the slot and letting CPlayerPed::ProcessWeaponSwitch make
		// the change.
		if(s_wantSlot[i] >= 0){
			if(s_wantSlot[i] == WEAPONSLOT_UNARMED || partner->HasWeaponSlot(s_wantSlot[i])){
				partner->m_nSelectedWepSlot = s_wantSlot[i];
				s_wantSlot[i] = -1;
			}else if(CTimer::GetTimeInMilliseconds() > s_wantSlotUntil[i])
				s_wantSlot[i] = -1;
		}
	}
}

// One ammo count per slot, shared by the whole party.  Player 1's m_nAmmoTotal
// is the pool; each partner's is a cache of it.  What every partner spent last
// frame is measured against their cache here, at the top of the frame, summed
// and taken out of the pool -- so a burst from one empties the others too, and
// the same rounds cannot be fired twice.
//
// The clip is each player's own and is clamped to what the pool has left: a
// magazine cannot hold rounds the others have already spent.  The reload
// timer is untouched, so a reload still takes its own time.
void
SyncSharedAmmo(CPlayerPed *lead)
{
	const int frenzySlot = CDarkel::GetFrenzyWeaponSlot();

	for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
		CWeapon &leadWeapon = lead->GetWeapon(slot);
		const eWeaponType leadType = leadWeapon.m_eWeaponType;
		// Only a weapon the party holds, and never the rampage's, which is
		// player 1's alone and must not arm the partners when it ends.
		if(slot == frenzySlot || !MirrorsWeapon(leadType)){
			for(int i = 1; i < NUMPLAYERS; i++)
				s_poolAmmo[i][slot] = -1;
			continue;
		}

		int32 spent = 0;
		for(int i = 1; i < NUMPLAYERS; i++){
			CPlayerPed *partner = CWorld::Players[i].m_pPed;
			if(partner == nil)
				continue;
			CWeapon &partnerWeapon = partner->GetWeapon(slot);
			if(partnerWeapon.m_eWeaponType != leadType){
				s_poolAmmo[i][slot] = -1;
				continue;
			}
			if(s_poolAmmo[i][slot] < 0){
				// First frame this partner holds it: they start from the
				// pool rather than from whatever their own count happened to
				// be.
				s_poolAmmo[i][slot] = leadWeapon.m_nAmmoTotal;
			}else{
				const int32 used = s_poolAmmo[i][slot] - partnerWeapon.m_nAmmoTotal;
				if(used > 0)
					spent += used;
			}
		}
		if(spent > 0)
			leadWeapon.m_nAmmoTotal = Max(0, leadWeapon.m_nAmmoTotal - spent);

		for(int i = 1; i < NUMPLAYERS; i++){
			CPlayerPed *partner = CWorld::Players[i].m_pPed;
			if(partner == nil)
				continue;
			CWeapon &partnerWeapon = partner->GetWeapon(slot);
			if(partnerWeapon.m_eWeaponType != leadType)
				continue;
			partnerWeapon.m_nAmmoTotal = leadWeapon.m_nAmmoTotal;
			if(leadWeapon.m_nAmmoInClip > leadWeapon.m_nAmmoTotal)
				leadWeapon.m_nAmmoInClip = leadWeapon.m_nAmmoTotal;
			if(partnerWeapon.m_nAmmoInClip > partnerWeapon.m_nAmmoTotal)
				partnerWeapon.m_nAmmoInClip = partnerWeapon.m_nAmmoTotal;
			// A weapon a partner emptied and left OUT_OF_AMMO comes back to
			// life when the pool is refilled.  Player 1's own is reset by
			// CPed::GiveWeapon when the pickup lands; the partners are not
			// given anything, so it has to be done here or they would never
			// be able to fire it again.
			if(partnerWeapon.m_eWeaponState == WEAPONSTATE_OUT_OF_AMMO && partnerWeapon.m_nAmmoTotal > 0)
				partnerWeapon.m_eWeaponState = WEAPONSTATE_READY;
			s_poolAmmo[i][slot] = partnerWeapon.m_nAmmoTotal;
		}
	}
}

// --- putting a partner into the world, and taking them out -------------------
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
		// Peds count too, unlike the usual object-only test: partners
		// arriving together are kept from standing in each other, and from
		// standing in whoever is already there.
		if(CWorld::TestSphereAgainstWorld(pos, 0.35f, nil, true, true, true, true, false, false) != nil)
			continue;
		out = pos;
		return true;
	}
	return false;
}

// Which vehicle a partner who is hopping straight into a seat asked for, one
// row per player.  It only lives from the frame the enter button is read to
// the spawn a moment later, and it is a registered reference so a car that is
// gone by then reads as nil rather than dangling.
CVehicle *s_rideInto[NUMPLAYERS];

// Which passenger seat a teleport may use.  Seat 0 is the front passenger
// door (RF), 1 the rear left and 2 the rear right -- the seats
// CPed::PedSetInCarCB hands to exactly those doors.  A seat whose door is
// flagged is one somebody is walking to: taking it by teleport would leave
// that walker finishing their door animation with no seat, marked as in the
// car and in nobody's passenger list.  A bus has no door-to-seat map (its
// walkers take the first free seat), so there the plain rule stands.
bool
FreeTeleportSeat(CVehicle *vehicle, uint8 &seat)
{
	static const int8 kSeatFlags[3] = { CAR_DOOR_FLAG_RF, CAR_DOOR_FLAG_LR, CAR_DOOR_FLAG_RR };
	for(int i = 0; i < vehicle->m_nNumMaxPassengers; i++){
		if(vehicle->pPassengers[i] != nil)
			continue;
		if(!vehicle->bIsBus && i < 3 && (vehicle->m_nGettingInFlags & kSeatFlags[i]))
			continue;
		seat = (uint8)i;
		return true;
	}
	return false;
}

// Whether a player can sit in this vehicle while another player drives it.
// Only ever a vehicle a player is driving: the engine makes any player who
// gets onto a bike or into a boat its driver whatever seat they asked for,
// and a car with a player in it and nobody at the wheel is not one the
// traffic code expects.
bool
CanRideAlong(CPlayerPed *driver, CVehicle *vehicle)
{
	if(vehicle == nil || vehicle->pDriver != driver)
		return false;
	if(driver->m_nPedState != PED_DRIVING || driver->m_objective == OBJECTIVE_LEAVE_CAR)
		return false;
	if(!vehicle->IsCar() && !vehicle->IsBike())
		return false;
	if(vehicle->GetStatus() == STATUS_WRECKED || vehicle->bIsInWater || vehicle->IsUpsideDown())
		return false;
	uint8 seat;
	return FreeTeleportSeat(vehicle, seat);
}

// Whether a player can sit in this vehicle while a player drives it, however
// that driver is found.
bool
CanRideAlongAnyPlayer(CVehicle *vehicle)
{
	return vehicle != nil && vehicle->pDriver != nil && vehicle->pDriver->IsPlayer() &&
		CanRideAlong((CPlayerPed*)vehicle->pDriver, vehicle);
}

// A seat in a vehicle one of the other partners is in and a player is driving,
// for a partner who has none in player 1's.  Nil if there is not one.
CVehicle *
FindPartnerCarWithSeat(int player)
{
	for(int i = 1; i < NUMPLAYERS; i++){
		if(i == player)
			continue;
		CPlayerPed *other = CWorld::Players[i].m_pPed;
		if(other != nil && other->bInVehicle && CanRideAlongAnyPlayer(other->m_pMyVehicle))
			return other->m_pMyVehicle;
	}
	return nil;
}

// Whether the pool of references has room for the few a partner takes.
// RegisterReference gives up without a word when the pool is empty, and a
// pointer that was meant to be nilled when its target is deleted then dangles:
// the partner's own slot in CWorld::Players, the vehicle they sit in, the one
// they are about to hop into.  So whoever is about to register those asks first,
// and tries again later if it is not so.  Eight is the few with some to spare;
// a partner put at the wheel of a boat takes about a dozen, and asks for twenty.
bool
ReferencesToSpare(int wanted = 8)
{
	int spare = 0;
	for(CReference *ref = CReferences::pEmptyList; ref != nil && spare < wanted; ref = ref->next)
		spare++;
	return spare >= wanted;
}

// Straight into a seat, the way COMMAND_CREATE_CHAR_AS_PASSENGER seats a ped
// it has just made.  Deliberately not CPed::WarpPedIntoCar, which marks the car
// as the player's to drive for any ped that IsPlayer().  The seat is the first
// one no one is walking to; AddPassenger refuses an occupied one either way, so
// two teleports can never share a seat.
bool
SeatPartner(CPlayerPed *partner, CVehicle *vehicle)
{
	uint8 seat;
	if(!FreeTeleportSeat(vehicle, seat))
		return false;
	if(!vehicle->AddPassenger(partner, seat))
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

// At the wheel of a boat the smuggling run has made for them, the way
// COMMAND_WARP_PLAYER_INTO_CAR seats player 1 (Script4.cpp): the order to drive
// it, then CPed::WarpPedIntoCar, which makes them its driver, puts the boat in
// the player state and gives them the driving animation.  This is the one place
// WarpPedIntoCar is wanted: a boat is a vehicle a player is always the driver
// of.  The order was only the key to that, so it is taken off again; a normal
// entry ends with none.
bool
SeatPartnerAsDriver(CPlayerPed *partner, CVehicle *boat)
{
	if(boat->pDriver != nil || boat->GetStatus() == STATUS_WRECKED)
		return false;
	partner->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, boat);
	// WarpPedIntoCar does nothing but sit the ped down unless that order took.
	if(partner->m_objective != OBJECTIVE_ENTER_CAR_AS_DRIVER)
		return false;
	partner->WarpPedIntoCar(boat);
	partner->SetObjective(OBJECTIVE_NONE);
	return partner->bInVehicle && boat->pDriver == partner;
}

void
RemovePartner(int player, const char *why, bool carry)
{
	CPlayerPed *partner = CWorld::Players[player].m_pPed;
	s_downTime[player] = 0;
	s_reviveTime[player] = 0;
	// A revive that has not spawned yet dies with the partner: the next arrival
	// is beside player 1 again, not at a body that is no longer there.  This is
	// what keeps a session pause -- a mission, player 1 going down -- between
	// the revive and the spawn from leaving the position behind.
	s_reviveValid[player] = false;
	// And so does the count of how long they have held player 1 at the wall.
	s_wallSince[player] = 0;
	s_wallLast[player] = 0;
	s_aim[player].active = false;
	s_aim[player].drawReticle = false;
	// A hop-in that has not spawned yet goes with them too.
	if(s_rideInto[player] != nil)
		s_rideInto[player]->CleanUpOldReference((CEntity**)&s_rideInto[player]);
	s_rideInto[player] = nil;
	if(partner == nil)
		return;

	s_carry[player].valid = carry && !partner->DyingOrDead();
	if(s_carry[player].valid){
		s_carry[player].health = partner->m_fHealth;
		s_carry[player].armour = partner->m_fArmour;
		s_carry[player].slot = partner->m_nSelectedWepSlot;
		for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++)
			s_carry[player].weapon[slot] = partner->GetWeapon(slot).m_eWeaponType;
	}

	// The destructor below gives back the one door the ped was using.  Stepping
	// off the back of a bike holds two, and the one left behind would keep that
	// bike from ever being cleared away or ridden pillion again.
	if(partner->m_nPedState == PED_EXIT_CAR && partner->m_pMyVehicle != nil && partner->m_pMyVehicle->IsBike())
		partner->m_pMyVehicle->m_nGettingOutFlags &= ~(CAR_DOOR_FLAG_LR | CAR_DOOR_FLAG_RR);

	// A driver takes the car's status with them.  The destructor lets go of the
	// seat and nothing more: the car would stay in STATUS_PLAYER with nobody in
	// it, holding its last throttle and steering, answering to player 1's pad if
	// it is a bike or a helicopter, and in a boat asking the missing driver for
	// a gun.  The engine abandons the car when it removes a player from one
	// (CPed::FlagToDestroyWhenNextProcessed); so does this.
	CVehicle *vehicle = partner->m_pMyVehicle;
	if(partner->InVehicle() && vehicle->pDriver == partner && vehicle->GetStatus() != STATUS_WRECKED)
		vehicle->SetStatus(STATUS_ABANDONED);
	// A partner busted as a passenger froze the car they were in (CCopPed::
	// SetArrestPlayer: handbrake on, STATUS_PLAYER_DISABLED), and with them gone
	// nothing lets it go.  If a player is at the wheel and is not being arrested
	// themselves, the car is theirs again.
	else if(partner->InVehicle() && vehicle->GetStatus() == STATUS_PLAYER_DISABLED &&
	        vehicle->pDriver != nil && vehicle->pDriver != partner && vehicle->pDriver->IsPlayer() &&
	        !vehicle->pDriver->DyingOrDead() && ((CPlayerPed*)vehicle->pDriver)->m_pArrestingCop == nil){
		vehicle->bIsHandbrakeOn = false;
		vehicle->SetStatus(STATUS_PLAYER);
	}

	COOP_LOG("WII coop: player %d out (%s)\n", player + 1, why);
	// The destructor does the rest: out of the world, out of whatever seat or
	// doorway they were in, fire out, weapons freed.  It is the same path the
	// engine takes for any ped it removes, which is the point of deleting rather
	// than hiding -- there is no half-present player for a pool scan to find.
	CWorld::RemoveReferencesToDeletedObject(partner);
	delete partner;
	CWorld::Players[player].m_pPed = nil;
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

// Whether any partner has joined, in the world or not: a party that a mission
// is holding has nobody in the world, and is still a party.
bool
AnyJoined(void)
{
	for(int partner = 0; partner < kNumPartners; partner++)
		if(s_joined[partner])
			return true;
	return false;
}

// Whether somebody did something to a controller this frame, join button or not:
// any button going down, or a stick pushed out of the middle.  For telling a
// player who is pressing things on a controller that is doing nothing for them
// what it would take.
bool
PadTouched(const CControllerState &now, const CControllerState &was)
{
	if((now.Start && !was.Start) || (now.Select && !was.Select) ||
	   (now.Cross && !was.Cross) || (now.Circle && !was.Circle) ||
	   (now.Square && !was.Square) || (now.Triangle && !was.Triangle) ||
	   (now.LeftShoulder1 && !was.LeftShoulder1) || (now.LeftShoulder2 && !was.LeftShoulder2) ||
	   (now.RightShoulder1 && !was.RightShoulder1) || (now.RightShoulder2 && !was.RightShoulder2) ||
	   (now.LeftShock && !was.LeftShock) || (now.RightShock && !was.RightShock) ||
	   (now.DPadUp && !was.DPadUp) || (now.DPadDown && !was.DPadDown) ||
	   (now.DPadLeft && !was.DPadLeft) || (now.DPadRight && !was.DPadRight))
		return true;
	// Half way out is a push; a stick resting off centre is not.
	const int16 kPush = 64;
	return (Abs(now.LeftStickX) > kPush && Abs(was.LeftStickX) <= kPush) ||
	       (Abs(now.LeftStickY) > kPush && Abs(was.LeftStickY) <= kPush) ||
	       (Abs(now.RightStickX) > kPush && Abs(was.RightStickX) <= kPush) ||
	       (Abs(now.RightStickY) > kPush && Abs(was.RightStickY) <= kPush);
}

// A partner who has to wait to be put into the world because of where player 1
// is: not yet, rather than not at all.  Called every time an attempt is turned
// away for that, and says why once the wait has lasted a moment.  A boat or an
// aircraft is a wait of its own kind from a car with no room in it, and each has
// its own line.
void
NoteWaiting(int player, CVehicle *vehicle)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();
	if(s_waitSince[player] == 0 || now - s_waitLast[player] > 3*kRetryMs){
		s_waitSince[player] = now;
		s_waitTold[player] = false;
	}
	s_waitLast[player] = now;
	if(s_waitTold[player] || now - s_waitSince[player] < kWaitNoticeMs)
		return;
	const bool ride = vehicle->IsBoat() || vehicle->IsHeli() || vehicle->IsPlane() ||
		vehicle->IsRealHeli() || vehicle->IsRealPlane();
	s_waitTold[player] = TellAbout(player, ride ? kRideKeys[player - 1] : kSeatKeys[player - 1]);
}

// What the first partner to arrive is told, after the line that they have joined:
// that the ammo and the money are the whole party's, and which button is theirs
// for the camera and what leaving takes.  The controls differ by controller, so
// there is a line for each kind.  They are as true in a minute as now, so they
// wait for the help box as long as they have to.  The camera button is the one a
// partner has to themselves (player 1's picks the framing); verified against
// captureWiimote, captureClassic and captureGameCube in WiiPad.cpp.  Leaving is
// the controller going away, which is all CCoop::Update takes for it: the remote
// switched off, its Nunchuk pulled out, the pad unplugged.
void
TeachParty(int player)
{
	Tell("WII_SHA", kNoticeStaleLongMs);
#ifdef NINTENDO_WII
	const char *tip = nil;
	switch(WiiPadQueryPlayer(player)){
	case WII_PAD_GAMECUBE:
		tip = "WII_TPG";
		break;
	case WII_PAD_CLASSIC:
		tip = "WII_TPC";
		break;
	case WII_PAD_WIIMOTE_NUNCHUK:
		tip = "WII_TPW";
		break;
	default:
		break;
	}
	if(tip != nil)
		Tell(tip, kNoticeStaleLongMs);
#endif
}

bool
SpawnPartner(CPlayerPed *lead, int player)
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
	// The partner's slot in CWorld::Players and the vehicle they sit in are both
	// registered references; see ReferencesToSpare.
	if(!ReferencesToSpare())
		return false;

	// The characters are special models, not model slots of their own: the name
	// is loaded into the slot that goes with its number in the game's own list
	// (CStreaming::RequestSpecialChar), the same slots the cutscenes spawn their
	// characters from -- which is safe here because a cutscene, like a mission,
	// stands co-op down and takes the partners with it.  Looking the name up as
	// a model finds nothing, which is how a chosen skin used to come out as
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
	// Asked for here and waited for, not loaded on the spot: the streamer reads
	// it in over the next frames like anything else, and this is tried again
	// each time round (kRetryMs) until it is there.  LoadAllRequestedModels
	// would block the game on the whole of the streaming queue, on every regroup,
	// hop-in and revive of a partner who has a skin.  The request is repeated
	// each try because it is cheap, and because it keeps a model that is already
	// in memory from being the one the streamer drops next.  The partner is not
	// made until it is here, so a revive's place and a hop-in's car, which are
	// worked out below, are still there to be used.
	int skinModel = -1;
	const int skin = WiiCoopSkin[player - 1];
	if(skin > 0 && skin < (int)ARRAY_SIZE(kCoopSkinChars) && kCoopSkinChars[skin] >= 0){
		const int charId = kCoopSkinChars[skin];
		CStreaming::RequestSpecialChar(charId, kCoopSkinModels[skin], STREAMFLAGS_DEPENDENCY);
		if(CStreaming::HasModelLoaded(MI_SPECIAL01 + charId)){
			skinModel = MI_SPECIAL01 + charId;
			s_skinWaitSince[player] = 0;
		}else{
			const uint32 now = CTimer::GetTimeInMilliseconds();
			if(s_skinWaitSince[player] == 0 || now - s_skinWaitLast[player] > 3*kRetryMs)
				s_skinWaitSince[player] = now;
			s_skinWaitLast[player] = now;
			if(now - s_skinWaitSince[player] < kSkinWaitMs)
				return false;
			// Given up on: they come in as the ordinary player model.
			COOP_LOG("WII coop: player %d's skin did not stream in\n", player + 1);
			s_skinWaitSince[player] = 0;
		}
	}

	CVehicle *ride = nil;
	// Whether the seat is the wheel of a boat the smuggling run made for them.
	bool atWheel = false;
	CVector pos;
	if(s_reviveValid[player]){
		// Brought back where they fell, not beside player 1 and not into their
		// car: the ground the fight was on is the point of a revive.
		pos = s_reviveAt[player];
		s_reviveValid[player] = false;
	}else{
		// A car this partner asked to hop into a frame or two ago -- any
		// player's, not just player 1's.  A registered reference, so one that
		// has gone away since is simply nil.
		ride = s_rideInto[player];
		if(ride != nil){
			ride->CleanUpOldReference((CEntity**)&s_rideInto[player]);
			s_rideInto[player] = nil;
			if(!CanRideAlongAnyPlayer(ride))
				ride = nil;
		}
		if(ride != nil){
			pos = ride->GetPosition();
		}else if(lead->bInVehicle && lead->m_pMyVehicle){
			CVehicle *vehicle = lead->m_pMyVehicle;
			if(CanRideAlong(lead, vehicle)){
				ride = vehicle;
				pos = vehicle->GetPosition();
			}else if((ride = FindPartnerCarWithSeat(player)) != nil){
				// No seat in player 1's, but another player is driving a car that
				// has one: the party's cars are all part of it.
				pos = ride->GetPosition();
			}else if(vehicle->IsBoat() && ReferencesToSpare(20) &&
			         (ride = CCoopModes::EscortBoat(player, vehicle)) != nil){
				// Player 1 is at sea on a smuggling run, and a boat has no seat to
				// give: the run makes this partner a boat of their own to drive.
				atWheel = true;
				pos = ride->GetPosition();
			}else{
				// No seat.  Beside it if it is standing still, and otherwise not
				// yet: they join when player 1 stops or gets out.  Which, to the
				// person holding the controller, is a character who has vanished,
				// so they are told.
				if(vehicle->IsBoat() || vehicle->m_vecMoveSpeed.MagnitudeSqr() > SQR(kSlowVehicle)){
					NoteWaiting(player, vehicle);
					return false;
				}
				if(!FindSpotBeside(vehicle->GetPosition(), vehicle->GetRight(), vehicle->GetForward(), 3.0f, pos))
					return false;
			}
		}else{
			if(!FindSpotBeside(lead->GetPosition(), lead->GetRight(), lead->GetForward(), 1.6f, pos))
				return false;
		}
	}

	// Making a player ped resets the things that were only ever reset when THE
	// player was made: among them the speed the game runs at, which a cheat or
	// a script may have changed and would find put back every time a partner
	// came round a corner.
	const float timeScale = CTimer::GetTimeScale();
	CPlayerPed *partner = new CPlayerPed();
	CTimer::SetTimeScale(timeScale);
	if(skinModel >= 0){
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
	// Not PEDTYPE_PLAYER1, and that is what keeps this ped out of the save:
	// CPools::SavePedPool picks what to write by exactly that type, and
	// LoadPedPool would hand a second one to CWorld::Players[0].  It still
	// answers true to IsPlayer(), which is what the rest of the engine asks.
	partner->m_nPedType = PEDTYPE_PLAYER2 + (player - 1);
	// What COMMAND_CREATE_PLAYER does for player 1, and what protects them: a
	// RANDOM_CHAR is fair game for CPopulation::RemovePedsIfThePoolGetsFull, and
	// a vehicle with only RANDOM_CHARs aboard is one the engine may delete.
	partner->CharCreatedBy = MISSION_CHAR;
	// Stay in the car when a player gets back into it, instead of being told
	// to get out like any other passenger.  They leave when they press the
	// button.
	partner->bStayInCarOnJack = true;
	// Nor pulled out of it: a player coming to the passenger door of a car with
	// a partner already in that seat would otherwise haul them into the road
	// to get past.
	partner->bDontDragMeOutCar = true;

	CWorld::Players[player].m_pPed = partner;
	partner->RegisterReference((CEntity**)&CWorld::Players[player].m_pPed);

	partner->SetOrientation(0.0f, 0.0f, 0.0f);
	partner->SetPosition(pos);
	partner->m_fRotationCur = lead->m_fRotationCur;
	partner->m_fRotationDest = lead->m_fRotationCur;
	partner->SetHeading(lead->m_fRotationCur);
	partner->m_area = lead->m_area;
	partner->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	CWorld::Add(partner);
	partner->m_wepAccuracy = 100;

	if(s_carry[player].valid){
		partner->m_fHealth = s_carry[player].health;
		partner->m_fArmour = s_carry[player].armour;
		s_wantSlot[player] = s_carry[player].slot;
		// Their weapons come back through the same queue player 1's gains do,
		// on top of anything that was still on its way to them when they went.
		// Empty, because the ammo is the shared pool, not a count a partner
		// owns; the first sync fills them from it.
		//
		// Only what player 1 still holds in that slot.  A weapon they have used
		// up or lost since has no pool left to fill it from, and would come back
		// as a gun with nothing in it.  The rampage's slot is the exception: what
		// player 1 holds there is the rampage's gun, and their own comes back
		// when it ends, with nothing to say it has.
		const int frenzySlot = CDarkel::GetFrenzyWeaponSlot();
		for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
			if(s_carry[player].weapon[slot] == WEAPONTYPE_UNARMED)
				continue;
			if(slot != frenzySlot && lead->GetWeapon(slot).m_eWeaponType != s_carry[player].weapon[slot])
				continue;
			if(s_owedType[player][slot] == WEAPONTYPE_UNARMED)
				s_owedType[player][slot] = s_carry[player].weapon[slot];
		}
	}else{
		partner->m_fHealth = CWorld::Players[LEAD].m_nMaxHealth;
		partner->m_fArmour = lead->m_fArmour;
		s_wantSlot[player] = lead->m_currentWeapon;
		ClearArsenalFor(player);
		QueueArsenalFor(player);
	}
	s_carry[player].valid = false;
	// The new ped's ammo counts start from the pool, not from a spend measured
	// against the last ped to hold this slot.
	ForgetSharedAmmo(player);
	// The party shares its heat: a partner who arrives while player 1 is
	// wanted is wanted too, and the mirror starts from there.
	partner->m_pWanted->SetWantedLevel(lead->m_pWanted->GetWantedLevel());
	s_wanted[LEAD] = lead->m_pWanted->GetWantedLevel();
	s_wanted[player] = partner->m_pWanted->GetWantedLevel();
	s_wantSlotUntil[player] = CTimer::GetTimeInMilliseconds() + 4000;

	if(ride != nil && !(atWheel ? SeatPartnerAsDriver(partner, ride) : SeatPartner(partner, ride))){
		// CanRideAlong found a seat a moment ago, so this cannot happen; if it
		// ever does, a player standing inside a car is not the way to find out.
		// (An escort boat was empty a moment ago too, and is the same.)
		RemovePartner(player, "no seat after all", false);
		return false;
	}

	s_aim[player].facing = false;
	s_aim[player].assistHandle = -1;
	COOP_LOG("WII coop: player %d in%s\n", player + 1,
	         atWheel ? " (escort boat)" : ride != nil ? " (riding along)" : "");
	s_waitSince[player] = 0;
	s_waitTold[player] = false;
	if(s_announceJoin[player]){
		s_announceJoin[player] = false;
		s_announceReturn[player] = false;
		Tell(kJoinedKeys[player - 1]);
		// The first arrival of the run also learns how the party works, once.
		if(!s_introTold){
			s_introTold = true;
			TeachParty(player);
		}
	}else if(s_announceReturn[player]){
		s_announceReturn[player] = false;
		TellAbout(player, kGroupKeys[player - 1]);
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

// The nearest car, bike or boat a partner could take as its driver.  All
// three come off the driver's own pad now; a bike makes whoever climbs on it
// the rider, and a boat makes whoever boards it the driver, which is what a
// partner wants in every case.  Never one a player is driving -- that would be
// a carjack of another player -- and not one a player is already walking to, a
// wreck or one on its roof, which nobody can enter.
CVehicle *
FindCarToSteal(CPlayerPed *partner)
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
		// The one a player is entering: two players finishing the same car's
		// enter as its driver would leave one of them in the seat with the
		// other's ped as the car's driver.  Tested on the state, not on
		// m_carInObjective alone: the field is left pointing at the last car
		// a player entered -- the game clears it only through ClearObjective,
		// which the player's own enter never goes through -- so a car they had
		// driven once could never be taken by a partner again.
		bool beingEntered = false;
		for(int i = 0; i < NUMPLAYERS && !beingEntered; i++){
			CPlayerPed *other = CWorld::Players[i].m_pPed;
			if(other != nil && other != partner &&
			   (other->m_nPedState == PED_ENTER_CAR || other->m_nPedState == PED_CARJACK) &&
			   vehicle == other->m_carInObjective)
				beingEntered = true;
		}
		if(beingEntered)
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
// only ever run for the player in focus.  This is a partner's share of it.
// On foot the button means one of two things, in this order:
//
//   * a free seat in whatever another player is driving or climbing into --
//     get in beside them, which is what it has always meant; or
//   * failing that, the nearest car within reach -- take it as its driver and
//     drive it.  That is the partner's own car, not anyone else's.
//
// Returns true when the partner should be put straight into a seat instead: the
// car is already moving, and nobody catches a moving car on foot.  rideInto is
// then the seat's vehicle -- it may be any player's, so the caller cannot work
// it out from player 1's car.
bool
UpdatePartnerVehicle(int player, CPlayerPed *partner, CPad *pad, CVehicle *&rideInto)
{
	rideInto = nil;
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

	// A seat with another player first -- player 1's car ahead of the rest,
	// because that is the car the party has always shared.  The car they are
	// in, or the one they are climbing into, and only if it is theirs or
	// empty and has room.
	for(int i = 0; i < NUMPLAYERS; i++){
		if(i == player)
			continue;
		CPlayerPed *other = CWorld::Players[i].m_pPed;
		if(other == nil)
			continue;
		CVehicle *vehicle = nil;
		if(other->bInVehicle)
			vehicle = other->m_pMyVehicle;
		else if(other->m_nPedState == PED_ENTER_CAR || other->m_nPedState == PED_CARJACK)
			vehicle = other->m_carInObjective;
		if(vehicle == nil || (vehicle->pDriver != nil && vehicle->pDriver != other) ||
		   (!vehicle->IsCar() && !vehicle->IsBike()) ||
		   vehicle->GetStatus() == STATUS_WRECKED || vehicle->bIsInWater ||
		   (vehicle->GetPosition() - partner->GetPosition()).Magnitude() > kBoardingRange)
			continue;
		uint8 seat = 0;
		if(!FreeTeleportSeat(vehicle, seat))
			continue;
		// Standing still, or nearly: walk to a door and get in like anybody
		// else.  Already rolling: straight into the seat.
		//
		// Onto a bike it is always straight into the seat, and only once its
		// rider is sitting on it.  The engine makes a bike the player's to
		// ride when any player finishes climbing on, whichever seat they
		// asked for -- so a partner who was still walking over when the rider
		// got off again would end up on the back of a bike with nobody on the
		// front, being ridden from the other player's pad, and the bike's
		// drive-by code looks up its rider without asking whether there is
		// one.
		if(vehicle->IsBike()){
			if(!CanRideAlong(other, vehicle))
				return false;
			rideInto = vehicle;
			return true;
		}
		if(vehicle->m_vecMoveSpeed.MagnitudeSqr() > SQR(0.04f) && CanRideAlong(other, vehicle)){
			rideInto = vehicle;
			return true;
		}
		partner->SetObjective(OBJECTIVE_ENTER_CAR_AS_PASSENGER, vehicle);
		return false;
	}

	// No seat with another player.  Take a car of their own instead.
	CVehicle *steal = FindCarToSteal(partner);
	if(steal != nil)
		partner->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, steal);
	return false;
}

// Whether this partner, on foot, has been holding player 1 at the wall long
// enough that they are the problem rather than the wall; see kWallHoldMs.
// CCoop::LimitSeparation keeps the count while player 1 pushes.  It restarts
// here when the pushing stops or the partner is seen to be going somewhere.
bool
HoldingLeadAtWall(int player, CPlayerPed *partner)
{
	if(s_wallSince[player] == 0)
		return false;
	const uint32 now = CTimer::GetTimeInMilliseconds();
	if(now - s_wallLast[player] > kWallGapMs || partner->bInVehicle ||
	   (partner->GetPosition() - s_wallAt[player]).MagnitudeSqr() > SQR(kWallMoved)){
		s_wallSince[player] = 0;
		return false;
	}
	return now - s_wallSince[player] >= kWallHoldMs;
}

// Whether a partner has to be brought back to player 1.
const char *
NeedsRegroup(CPlayerPed *lead, CPlayerPed *partner, int player, bool leadTeleported)
{
	if(leadTeleported)
		return "player 1 was moved";

	CVehicle *partnerVehicle = partner->bInVehicle ? partner->m_pMyVehicle : nil;
	// An NPC at the wheel: a partner being driven away by the traffic is
	// brought back, since the party cannot follow.  A car any player is
	// driving -- the lead's or another partner's -- is part of the party, and
	// the leash below is what keeps it near player 1.
	if(partnerVehicle != nil && partnerVehicle->pDriver != nil &&
	   !partnerVehicle->pDriver->IsPlayer())
		return "driven off by someone else";

	const CVector leadPos = (lead->bInVehicle && lead->m_pMyVehicle) ? lead->m_pMyVehicle->GetPosition() : lead->GetPosition();
	const CVector partnerPos = partnerVehicle != nil ? partnerVehicle->GetPosition() : partner->GetPosition();
	if(partnerPos.z < leadPos.z - kFallLimit)
		return "fell";
	// Anyone in a vehicle a player is driving gets the longer leash, the driver
	// and their passengers alike: it is the car that goes too far, a passenger
	// cannot slow it, and one who was held to the walking leash would be
	// deleted and put back beside player 1 well short of the point where the
	// driver, in the same car, is not.  The driver at the wheel is a player by
	// now -- an NPC at it was dealt with above.  Everybody else, on foot or in
	// a vehicle with nobody driving it, gets the walking one.
	// A pursuit mode suspends the distance leash -- falls and kidnaps still
	// bring a partner back, distance no longer does.
	const float leash = (partnerVehicle != nil && partnerVehicle->pDriver != nil) ? kLeashDriving : kLeash;
	if(!CCoopModes::FreeRoam() && (leadPos - partnerPos).Magnitude() > leash)
		return "too far from player 1";
	// The wall holds player 1 to this partner, and the partner is not coming.
	if(!CCoopModes::FreeRoam() && HoldingLeadAtWall(player, partner))
		return "holding player 1 at the wall";
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
		if(vehicle->pDriver == ped){
			// An aircraft's pilot has no gun to point: the drive-by is gated
			// off for them in CAutomobile::DoDriveByShootings, and the mounted
			// guns aim themselves.  A tank's reticle stays -- it is what aims
			// the turret.
			if(vehicle->IsRealHeli() || vehicle->IsRealPlane())
				return;
		}else{
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
CCoop::Notify(const char *key)
{
	Tell(key);
}

void
CCoop::Init(void)
{
	// Not the peds: whoever is resetting the game owns those.  CGame clears
	// CWorld::Players before the world is emptied, and the world frees every
	// ped in it.
	ms_bRunning = false;
	s_regroupLogTime = 0;
	s_leadPosValid = false;
	s_tetherTold = false;
	s_tetherAt = 0;
	s_missionHeld = false;
	s_missionTold = false;
	s_missionTellAt = 0;
	// (s_introTold is not reset: the controls are taught once per run.)
	// A partner's choice of camera does not outlive the game it was made in: the
	// partner it names may be a different player, or nobody, in the next.
	ms_nCameraFocus = -1;
	// Nor does a notice that was waiting its turn.
	s_noticeCount = 0;
	ForgetArsenal();
	for(int i = 0; i < NUMPLAYERS; i++){
		s_spawnTime[i] = 0;
		s_downTime[i] = 0;
		s_reviveTime[i] = 0;
		s_reviveValid[i] = false;
		s_wallSince[i] = 0;
		s_wallLast[i] = 0;
		s_skinWaitSince[i] = 0;
		s_announceJoin[i] = true;
		s_announceReturn[i] = false;
		s_situationAt[i] = 0;
		s_waitSince[i] = 0;
		s_waitLast[i] = 0;
		s_waitTold[i] = false;
		s_carry[i].valid = false;
		s_wantSlot[i] = -1;
		s_wanted[i] = 0;
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
	for(int partner = 0; partner < kNumPartners; partner++){
		s_padHere[partner] = false;
		s_padEver[partner] = false;
		s_joined[partner] = false;
		s_joinTold[partner] = false;
		s_joinHintAt[partner] = 0;
		s_batteryTold[partner] = false;
		s_padSince[partner] = 0;
		s_padLast[partner] = 0;
	}
	for(int player = 0; player < NUMPLAYERS; player++){
		if(s_rideInto[player] != nil)
			s_rideInto[player]->CleanUpOldReference((CEntity**)&s_rideInto[player]);
		s_rideInto[player] = nil;
	}
	// The minigames hold entities of their own; a reset is the end of them.
	CCoopModes::Init();
}

CPlayerPed *
CCoop::GetPlayerPed(int player)
{
	if(player < 0 || player >= NUMPLAYERS)
		return nil;
	return CWorld::Players[player].m_pPed;
}

bool
CCoop::IsJoined(int player)
{
	if(player == LEAD)
		return true;
	return player > LEAD && player < NUMPLAYERS && s_joined[player - 1];
}

bool
CCoop::IsControllerSilent(int player)
{
	if(player <= LEAD || player >= NUMPLAYERS)
		return false;
	const int partner = player - 1;
	// The same signal the six-second removal in Update runs on: the pad layer
	// reporting the controller as gone (its remote disconnected, its Nunchuk
	// pulled out, the pad unplugged), not anything about buttons or sticks, so a
	// player standing still with a working controller is never silent.  Just
	// earlier: that waits kDropAfterMs, this kSilentMs.
	return s_joined[partner] && s_padEver[partner] && !s_padHere[partner] &&
		CTimer::GetTimeInMilliseconds() - s_padLast[partner] > kSilentMs;
}

bool
CCoop::PairActive(void)
{
	if(!ms_bRunning)
		return false;
	for(int partner = 0; partner < kNumPartners; partner++)
		if(s_joined[partner])
			return true;
	return false;
}

int
CCoop::GetPlayerIndex(const CEntity *entity)
{
	if(entity == nil)
		return -1;
	for(int i = 0; i < NUMPLAYERS; i++){
		if(entity == CWorld::Players[i].m_pPed)
			return i;
	}
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
	if(partner < 0 || partner >= kNumPartners)
		return;
	const uint32 now = CTimer::GetTimeInMilliseconds();
	if(!present){
		s_padHere[partner] = false;
		return;
	}
	if(!s_padHere[partner]){
		s_padHere[partner] = true;
		s_padSince[partner] = now;
	}
	s_padLast[partner] = now;
	s_padEver[partner] = true;
}

void
CCoop::ReportPointer(int player, float x, float y)
{
	if(player < 0 || player >= NUMPLAYERS)
		return;
	s_aim[player].pointerX = Clamp(x, 0.0f, 1.0f);
	s_aim[player].pointerY = Clamp(y, 0.0f, 1.0f);
	s_aim[player].pointerTime = CTimer::GetTimeInMilliseconds();
	s_aim[player].pointerSeen = true;
}

void
CCoop::Suspend(const char *why)
{
	const uint32 until = CTimer::GetTimeInMilliseconds() + 1500;
	for(int player = 1; player < NUMPLAYERS; player++){
		RemovePartner(player, why, true);
		s_spawnTime[player] = until;
	}
}

void
CCoop::Update(void)
{
	const uint32 now = CTimer::GetTimeInMilliseconds();
	// Whatever was told last frame, if the help box is done with its last line.
	// First, so it happens whether or not a session is running: the notice that
	// co-op has stood down for a mission is told as the session ends.
	ShowNextNotice();
	const char *blocker = SessionBlocker();
	const bool allowed = blocker == nil;
	if(allowed != ms_bRunning){
		ms_bRunning = allowed;
		COOP_LOG("WII coop: session %s%s\n", allowed ? "on" : "off: ", allowed ? "" : blocker);
		for(int player = 1; player < NUMPLAYERS; player++)
			s_spawnTime[player] = now;
		s_leadPosValid = false;
	}

	// Someone who was playing a moment ago is owed a word about where they went.
	// Only for a mission: a cutscene explains itself, and the menu toggle was their
	// own doing.  Held from the first frame of the mission, whether or not the
	// session was running just before, but told only when the screen can show it:
	// the mission's own cutscene is usually on at the start and takes the help box
	// with it, so a line told on the first frame is a line nobody reads.  It waits
	// behind a hint of the mission's own, which the scripts have put up by then and
	// which says something more useful than this -- and is told again, not more
	// than once in kMissionRetellMs, whenever a partner's controller is touched,
	// since otherwise a partner's controller is dead for the whole mission with the
	// only explanation long gone.  When the party is back it says so.
	const bool onMission = blocker != nil && strcmp(blocker, "mission") == 0;
	if(onMission){
		if(!s_missionHeld && AnyJoined()){
			s_missionHeld = true;
			s_missionTold = false;
		}
		if(s_missionHeld && !TheCamera.m_WideScreenOn && !CCutsceneMgr::IsRunning() &&
		   !CCutsceneMgr::IsCutsceneProcessing()){
			bool touched = false;
			for(int partner = 0; partner < kNumPartners; partner++){
				CPad *pad = CPad::GetPad(PAD_COOP + partner);
				if(s_padHere[partner] && PadTouched(pad->NewState, pad->OldState))
					touched = true;
			}
			if(!s_missionTold || (touched && now - s_missionTellAt >= kMissionRetellMs)){
				s_missionTold = true;
				s_missionTellAt = now;
				Tell("WII_CCM");
			}
		}
	}else if(allowed){
		if(s_missionHeld && AnyJoined())
			Tell("WII_CPB");
		s_missionHeld = false;
	}else if(strcmp(blocker, "switched off") == 0)
		s_missionHeld = false;

	if(!ms_bRunning){
		// Nobody comes back from a mission, a cutscene or a death owing a line about
		// being brought back to the party.
		for(int player = 1; player < NUMPLAYERS; player++)
			s_announceReturn[player] = false;
		// A partner never outlives the session.  That is what "off during
		// missions" comes to in practice: by the time a mission's first scene
		// starts there is one player ped in the world, as its script assumes.
		// Player 1's own wasted or busted resets the party: the engine clears
		// his weapons at the hospital or the station, and the partners'
		// arsenal is his -- so they come back to whatever player 1 has, which
		// after that is nothing.  Every other way a session ends is a pause,
		// not a reset, and the partners keep what they carry.
		const bool reset = blocker != nil && strcmp(blocker, "player 1 down") == 0;
		for(int player = 1; player < NUMPLAYERS; player++)
			RemovePartner(player, blocker, !reset);
		s_aim[LEAD].active = false;
		s_aim[LEAD].drawReticle = false;
		// A focus a partner chose for a drive does not outlive the session.
		ms_nCameraFocus = -1;
		return;
	}

	CPlayerPed *lead = CWorld::Players[LEAD].m_pPed;
	CPad *leadPad = CPad::GetPad(0);

	// Whatever takes player 1's controls away takes the partners' with them,
	// and every pad reads its buttons the same way.
	for(int partner = 0; partner < kNumPartners; partner++){
		CPad *pad = CPad::GetPad(PAD_COOP + partner);
		pad->Mode = leadPad->Mode;
		pad->DisablePlayerControls = leadPad->DisablePlayerControls;
	}

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

	// A partner's camera button hands the shared camera's turn to their own
	// car: the camera can only follow one nose, and by default it follows the
	// car carrying the most players, so a partner driving their own car asks
	// for it with this.  Pressing it again gives the view back to the party.
	// Only partners have this button free: player 1's camera button is the
	// framing cycle above.
	if(PairActive() && !TheCamera.m_WideScreenOn){
		for(int partner = 0; partner < kNumPartners; partner++){
			CPad *pad = CPad::GetPad(PAD_COOP + partner);
			if(!pad->CycleCameraModeUpJustDown())
				continue;
			const int player = partner + 1;
			ms_nCameraFocus = (ms_nCameraFocus == player) ? -1 : player;
			COOP_LOG("WII coop: camera focus %d\n", ms_nCameraFocus);
			// The camera only follows a car its owner is driving (Cam.cpp), so
			// "following your car" is only true behind the wheel.  On foot, or in
			// somebody else's seat, the choice is kept for when they next drive.
			CPlayerPed *ped = CWorld::Players[player].m_pPed;
			const bool driving = ped != nil && ped->bInVehicle && ped->m_pMyVehicle != nil &&
				ped->m_pMyVehicle->pDriver == ped;
			Tell(ms_nCameraFocus < 0 ? "WII_CFA" : driving ? "WII_CFY" : "WII_CFD");
		}
	}

	// A script moving player 1 somewhere else -- into a building, to a safe
	// house -- leaves the partners where they were.  Travel never covers this
	// much ground in a frame, on foot or in anything.
	const CVector leadPos = (lead->bInVehicle && lead->m_pMyVehicle) ? lead->m_pMyVehicle->GetPosition() : lead->GetPosition();
	const bool leadTeleported = s_leadPosValid && (leadPos - s_leadPos).Magnitude() > kTeleportStep;
	s_leadPos = leadPos;
	s_leadPosValid = true;

	// The partners' pads: presence, the join button, and the leave.  One
	// partner joining or leaving is nobody else's business.
	bool padPresent[NUMPLAYERS];
	bool padGone[NUMPLAYERS];
	padPresent[LEAD] = false;
	padGone[LEAD] = false;
	for(int partner = 0; partner < kNumPartners; partner++){
		const int player = partner + 1;
		CPad *pad = CPad::GetPad(PAD_COOP + partner);
		padPresent[player] = s_padHere[partner] && now - s_padSince[partner] >= kJoinAfterMs;
		padGone[player] = !s_padHere[partner] && (!s_padEver[partner] || now - s_padLast[partner] > kDropAfterMs);
		if(padGone[player]){
			s_joined[partner] = false;
			s_joinTold[partner] = false;
			s_batteryTold[partner] = false;
			s_announceReturn[player] = false;
			// A camera focus was that partner's choice for their drive.  Their
			// pad is gone, so it goes with them: a partner who joins later
			// gets the party's car, not a camera that prefers someone else's.
			if(ms_nCameraFocus == player)
				ms_nCameraFocus = -1;
		}else if(padPresent[player] && !s_joined[partner]){
			// Any of these joins; they are A, B, 1, 2, Z, C and the D-pad on a
			// remote and Nunchuk, the face buttons, shoulders and D-pad on a Classic
			// Controller, and the same on a GameCube pad.  The prompt names A, the
			// one that is the same button on all three (Cross in every capture
			// function in WiiPad.cpp).  + and - are Start and Select, and the sticks
			// are the sticks, and none of them joins.
			const CControllerState &held = pad->NewState;
			if(held.Cross || held.Circle || held.Square || held.Triangle ||
			   held.LeftShoulder1 || held.LeftShoulder2 || held.RightShoulder1 || held.RightShoulder2 ||
			   held.DPadUp || held.DPadDown || held.DPadLeft || held.DPadRight ||
			   partner < ms_nAutoJoinPartners){
				s_joined[partner] = true;
#ifdef NINTENDO_WII
				WiiPadJoinPulse(player);
#endif
			}else{
				// Told when the controller is first noticed, and again whenever
				// it is pressed without joining, which is somebody trying to:
				// the first line goes by before the controller is in anyone's
				// hands, and a +, - or stick press otherwise gets no answer.
				const bool again = s_joinTold[partner] && now - s_joinHintAt[partner] >= kJoinHintMs &&
					PadTouched(held, pad->OldState);
				if(!s_joinTold[partner] || again){
					s_joinTold[partner] = true;
					s_joinHintAt[partner] = now;
					Tell(kJoinKeys[partner]);
				}
			}
		}
#ifdef NINTENDO_WII
		// A remote that is running out of batteries drops out in the middle of a
		// fight with no warning, and then "has left" with no cause.  Once per
		// connection; the flag is the remote's own, set when it reports its status.
		if(s_joined[partner] && !s_batteryTold[partner] && WiiPadBatteryLow(player)){
			s_batteryTold[partner] = true;
			Tell(kBatteryKeys[partner]);
		}
#endif
	}

	for(int player = 1; player < NUMPLAYERS; player++){
		CPlayerPed *partner = CWorld::Players[player].m_pPed;
		if(partner == nil)
			continue;
		CPad *partnerPad = CPad::GetPad(PAD_COOP + player - 1);
		const char *regroup;
		if(padGone[player]){
			RemovePartner(player, "controller gone", false);
			s_spawnTime[player] = now;
			s_announceJoin[player] = true;
			Tell(kLeftKeys[player - 1]);
		}else if(partner->DyingOrDead() || partner->m_nPedState == PED_ARRESTED ||
		         (partner->bInVehicle && partner->m_fHealth <= 1.0f)){
			// Nothing in the engine brings a player back except CGameLogic, and
			// CGameLogic only knows the player in focus.  So this is the whole of
			// a partner's wasted-and-busted: down where they fell, and then
			// either brought back by another player reaching them or, when the
			// bleed-out runs out, back beside player 1 as before.
			//
			// The last test is someone killed in a car.  CPed::InflictDamage does
			// not let them die there: it leaves them one point of health and an
			// order to get out and die on the pavement -- which a passenger cannot
			// carry out while the car is moving, so they would ride along at one
			// health for as long as player 1 kept driving.
			const bool busted = partner->m_nPedState == PED_ARRESTED;
			if(s_downTime[player] == 0){
				s_downTime[player] = now;
				// True for as long as it takes them to bleed out, and no longer.
				if(!busted)
					Tell(kDownKeys[player - 1], kNoticeStaleShortMs);
			}else if(!busted && SomeoneCanRevive(partner)){
				if(s_reviveTime[player] == 0)
					s_reviveTime[player] = now;
				else if(now - s_reviveTime[player] >= kReviveHoldMs){
					s_reviveTime[player] = 0;
					if(partner->DyingOrDead()){
						if(partner->bInVehicle && partner->m_pMyVehicle != nil){
							// Dead in a vehicle -- a car that drowned, a bike that
							// threw them.  Nowhere to stand them up, so this is the
							// ordinary return, beside player 1.
							RemovePartner(player, "revived", false);
						}else{
							// Dead on the ground: a fresh partner where the body is,
							// through the same machinery as every other arrival, and
							// with the party's set -- a partner's death is not a reset.
							const CVector at = partner->GetPosition();
							RemovePartner(player, "revived", false);
							s_reviveAt[player] = at;
							s_reviveValid[player] = true;
						}
						s_spawnTime[player] = now;
					}else{
						// Not dead yet: one point of health in a car, waiting to get
						// out and die.  Healed where they sit, and the order to die
						// taken off them.
						partner->m_fHealth = CWorld::Players[LEAD].m_nMaxHealth;
						if(partner->m_objective == OBJECTIVE_LEAVE_CAR_AND_DIE)
							partner->SetObjective(OBJECTIVE_NONE);
						partner->m_leaveCarTimer = 0;
						s_downTime[player] = 0;
					}
					Tell(kBackKeys[player - 1]);
				}
			}else{
				s_reviveTime[player] = 0;
				if(now - s_downTime[player] > kDownedBleedMs){
					RemovePartner(player, busted ? "busted" : "wasted", false);
					s_spawnTime[player] = now;
					// Back beside player 1 in a moment, with nothing on screen to say
					// where from: the arrival is told when it happens.
					s_announceReturn[player] = true;
				}
			}
		}else if((regroup = NeedsRegroup(lead, partner, player, leadTeleported)) != nil){
			// A partner who is driving their own car is moved with it.  The
			// leash fires because the world is only streamed around player 1, so
			// the party has to come back to them -- but the car is the thing that
			// went too far, and deleting a partner out of it throws away the
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
			// Each partner's search starts one notch further back than the
			// last, so three cars brought back at once line up along the road
			// instead of piling onto one node.
			const float lane = (float)(player - 1);
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
					for(float back = kRegroupBack*(1.0f + lane); back <= kRegroupBack*(3.0f + lane) && !boat; back += kRegroupBack){
						const CVector pos = leadAt - forward*back;
						float level;
						if(CWaterLevel::GetWaterLevel(pos, &level, true)){
							boatPos = CVector(pos.x, pos.y, level + 1.0f);
							boat = true;
						}
					}
					for(float ahead = kRegroupBack*(1.0f + lane); ahead <= kRegroupBack*(3.0f + lane) && !boat; ahead += kRegroupBack){
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
					airPos = leadAt - forward*(kRegroupBack*(1.0f + lane));
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
					// every second.  Behind is also the direction a partner can
					// drive on in, which is where they were going anyway.
					for(float back = kRegroupBack*(1.0f + lane); back <= kRegroupBack*(3.0f + lane) && node < 0; back += kRegroupBack){
						const int32 candidate = ThePaths.FindNodeClosestToCoors(leadAt - forward*back, PATH_CAR, kRegroupSearch);
						if(candidate < 0)
							continue;
						const CVector candidateAt = ThePaths.m_pathNodes[candidate].GetPosition();
						if((candidateAt - leadAt).Magnitude2D() < kRegroupClearance)
							continue;
						// Nor one the leash would catch the car at again at once.  The
						// search points are lanes of kRegroupBack apiece behind player
						// 1, and a node can be kRegroupSearch from its search point.
						if((candidateAt - leadAt).Magnitude() > kLeashDriving - kRegroupMargin)
							continue;
						node = candidate;
					}
					// Nothing clear behind: the node nearest player 1 after all, so
					// the car is at least on something.  It goes to the side of them
					// if that node is the one they are on, fanned out by lane so
					// two of them do not land on the same spot.  Still only if it is
					// near enough that the leash lets it be: a player 1 on foot in
					// the middle of a park can be a hundred metres from the nearest
					// road, and a car put down there would be hauled back to the
					// same node every frame.  With none near enough the car is not
					// moved and the partner is replaced, below.
					if(node < 0){
						node = ThePaths.FindNodeClosestToCoors(leadAt, PATH_CAR, 100.0f);
						if(node >= 0 && (ThePaths.m_pathNodes[node].GetPosition() - leadAt).Magnitude() > kLeashDriving - kRegroupMargin)
							node = -1;
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
					COOP_LOG("WII coop: player %d's boat brought back (%s)\n", player + 1, regroup);
				}
				TellAbout(player, kGroupKeys[player - 1]);
			}else if(airborne){
				driven->SetPosition(airPos);
				if(leadVehicle != nil)
					driven->SetOrientation(0.0f, 0.0f, leadVehicle->GetForward().Heading());
				driven->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
				driven->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
				if(now - s_regroupLogTime >= 3000){
					s_regroupLogTime = now;
					COOP_LOG("WII coop: player %d's aircraft brought back (%s)\n", player + 1, regroup);
				}
				TellAbout(player, kGroupKeys[player - 1]);
			}else if(node >= 0){
				CVector pos = ThePaths.m_pathNodes[node].GetPosition();
				if(beside)
					pos += leadVehicle->GetRight()*(kRegroupClearance + lane*4.0f);
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
					COOP_LOG("WII coop: player %d's car brought back (%s)\n", player + 1, regroup);
				}
				TellAbout(player, kGroupKeys[player - 1]);
			}else if(driven != nil && driven->IsBoat()){
				// Nowhere to put it: left alone rather than deleted.  The world
				// around it is thin, but a boat respawned on foot is worse.
				if(now - s_regroupLogTime >= 3000){
					s_regroupLogTime = now;
					COOP_LOG("WII coop: player %d's boat left where it is (no water by player 1)\n", player + 1);
				}
			}else{
				// Replaced rather than moved.  Moving a ped that might be halfway
				// through a door, a fall or a punch means unpicking whichever of
				// those it is; a fresh one beside player 1 is in a known state,
				// and carries over the health and the weapon the old one had.
				RemovePartner(player, regroup, true);
				s_spawnTime[player] = now;
				// Told when they arrive, which may be a moment off: there is a
				// seat or a skin to wait for.
				s_announceReturn[player] = true;
			}
		}else{
			s_downTime[player] = 0;
			// These are set on every passenger of a car whose driver is dragged
			// out, with no exception for players, and would send a partner
			// running from the car under the engine's control.
			partner->bFleeAfterExitingCar = false;
			partner->bHeldHostageInCar = false;
			partner->m_area = lead->m_area;
			// Another player's car is entered as a passenger or not at all.
			// A partner is sent to an empty seat, but if somebody takes it
			// while they walk over, CPed::SeekCar has them drag that somebody
			// out instead -- and CPed::PedSetInCarCB then makes the entering
			// partner the car's driver, over the top of the player who is
			// sitting in it.  Stopped here, on the frame it starts, before
			// anyone is pulled anywhere.
			//
			// A car a partner is taking as its driver is meant to be a
			// carjack, occupied or not -- cancelling every PED_CARJACK is what
			// stopped a partner stealing an occupied car at all -- but never
			// with a player in the driver's seat: that is not a carjack, it is
			// one player pulling another out of their own car.
			if(partner->m_nPedState == PED_CARJACK &&
			   (partner->m_objective == OBJECTIVE_ENTER_CAR_AS_PASSENGER ||
			    (partner->m_carInObjective != nil && partner->m_carInObjective->pDriver != nil &&
			     partner->m_carInObjective->pDriver->IsPlayer())))
				partner->QuitEnteringCar();
			CVehicle *rideInto = nil;
			if(UpdatePartnerVehicle(player, partner, partnerPad, rideInto) && ReferencesToSpare()){
				// Replaced, like every other time a partner is moved; the new
				// one is made in the seat.  SpawnPartner runs just below, and
				// the seat's vehicle goes with them: it may be any player's,
				// not just player 1's.  Not with the pool of references empty,
				// when the seat's vehicle would not be kept track of: the press
				// is let go, and the player presses again.
				RemovePartner(player, "hopping in", true);
				s_spawnTime[player] = now;
				s_rideInto[player] = rideInto;
				if(rideInto != nil)
					rideInto->RegisterReference((CEntity**)&s_rideInto[player]);
			}
		}
	}

	// The arsenal and the heat are the party's, whoever is in the world this
	// frame: the shared pool syncs every partner at once.
	UpdateArsenal(lead);
	SyncSharedAmmo(lead);
	ShareWantedLevel();

	for(int player = 1; player < NUMPLAYERS; player++){
		if(CWorld::Players[player].m_pPed != nil)
			continue;
		if(!padPresent[player] || !s_joined[player - 1] || now < s_spawnTime[player])
			continue;
		if(!SpawnPartner(lead, player))
			s_spawnTime[player] = now + kRetryMs;
	}

	for(int i = 0; i < NUMPLAYERS; i++)
		UpdateAim(i);
}

void
CCoop::LimitSeparation(CPed *ped, CVector2D &moved)
{
	if(!ms_bRunning)
		return;
	// A pursuit mode stands the wall aside; the parties in it have to be able
	// to separate.
	if(CCoopModes::FreeRoam())
		return;
	const int index = GetPlayerIndex(ped);
	if(index < 0)
		return;
	// The wall is a star: a partner is held to player 1, and player 1 is held
	// to each partner in turn -- with one partner that is exactly the old
	// rule, and with more it is each of the walls applied one after another,
	// so the lead cannot walk off and drag the party along.  Only between
	// players who are both up and both on foot.  Someone in a car is not held
	// by this -- the leash deals with them -- and someone lying dead in the
	// road does not get to keep the others standing over them.
	for(int i = 0; i < NUMPLAYERS; i++){
		if(i == index)
			continue;
		// Partners are not held to each other, only to player 1.
		if(index != LEAD && i != LEAD)
			continue;
		CPlayerPed *other = CWorld::Players[i].m_pPed;
		if(other == nil || other->bInVehicle || other->DyingOrDead() || other->m_nPedState == PED_ARRESTED)
			continue;

		CVector2D apart(ped->GetPosition().x - other->GetPosition().x, ped->GetPosition().y - other->GetPosition().y);
		const float distance = apart.Magnitude();
		if(distance <= kTetherStart)
			continue;
		apart = apart*(1.0f/distance);
		const float outward = moved.x*apart.x + moved.y*apart.y;
		if(outward <= 0.0f)
			continue;

		// All of the outward speed at the start of the band, none of it at the
		// end, so the wall is something a player runs out of rather than into.
		const float allowed = Clamp((kTetherMax - distance)/(kTetherMax - kTetherStart), 0.0f, 1.0f);
		moved.x -= apart.x*outward*(1.0f - allowed);
		moved.y -= apart.y*outward*(1.0f - allowed);

		// Told once the slowdown is plain, not only when it has all but stopped
		// them, and again every kTetherRetellMs for as long as somebody keeps
		// leaning on it: a line told once, mid-game, goes by while everyone is
		// watching the other player, and nothing else says why they are slowing.
		if(allowed < 0.7f){
			const uint32 now = CTimer::GetTimeInMilliseconds();
			if(!s_tetherTold || now - s_tetherAt >= kTetherRetellMs){
				s_tetherTold = true;
				s_tetherAt = now;
				Tell("WII_TTH");
			}
		}

		// Player 1 held at the edge by this partner.  Noted here, where the wall
		// is applied, and acted on in Update: this runs in the middle of the
		// world being processed, no place to be deleting a ped.  See
		// HoldingLeadAtWall.
		if(index == LEAD && allowed < 0.2f){
			const uint32 now = CTimer::GetTimeInMilliseconds();
			if(s_wallSince[i] == 0 || now - s_wallLast[i] > kWallGapMs){
				s_wallSince[i] = now;
				s_wallAt[i] = other->GetPosition();
			}
			s_wallLast[i] = now;
		}
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
	if(player < 0 || player >= NUMPLAYERS || !s_aim[player].drawReticle)
		return false;
	x = s_aim[player].reticleX;
	y = s_aim[player].reticleY;
	engaged = s_aim[player].engaged;
	return true;
}

bool
CCoop::GetDownedState(int player, float &bleedLeft, float &reviveProgress)
{
	// s_downTime is set when the partner is first seen down and cleared by the
	// revive, by the removal and by an Update pass that finds them well; the
	// ped test covers a ped the engine deleted since the last one.
	if(player <= LEAD || player >= NUMPLAYERS || CWorld::Players[player].m_pPed == nil || s_downTime[player] == 0)
		return false;
	const uint32 now = CTimer::GetTimeInMilliseconds();
	bleedLeft = Clamp(1.0f - (float)(now - s_downTime[player])/(float)kDownedBleedMs, 0.0f, 1.0f);
	reviveProgress = s_reviveTime[player] == 0 ? 0.0f :
		Clamp((float)(now - s_reviveTime[player])/(float)kReviveHoldMs, 0.0f, 1.0f);
	return true;
}
