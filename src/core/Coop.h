#pragma once

class CEntity;
class CPad;
class CPed;
class CPlayerPed;
class CVector;
class CVector2D;

// Couch co-op: up to three partners on the same screen, GTA: San Andreas PS2
// style.  The design, and the reasoning behind every rule below, is in
// COUCH-COOP-DESIGN.md.  This class is the session: when co-op is in charge,
// who the partners are, and where each player is aiming.
//
// Three things it deliberately does NOT do, because the rest of the engine
// relies on them:
//
//   It never changes CWorld::PlayerInFocus.  FindPlayerPed() is player 1 for
//   as long as the game runs, so scripts, the HUD, pickups and the save all go
//   on talking to the player they have always talked to.
//
//   It never runs during a mission.  The session is off for as long as
//   CTheScripts::IsPlayerOnAMission() is true, and the partners are taken out
//   of the world for that time rather than left standing in a scripted scene.
//
//   It never trusts its own pointers to the partners.  A partner's ped lives
//   in its CWorld::Players slot as a registered reference, which the engine
//   nils the moment the ped is deleted -- and the engine may delete it, for
//   instance along with a vehicle it was sitting in.  Everything here re-reads
//   those slots and treats nil as "bring them back", so there is nothing to
//   dangle.  The partners are slots 1..NUMPLAYERS-1, in PEDTYPE_PLAYER2..4;
//   slot 0 is player 1.
//
// The co-op options that are not pad settings: whether a player's shots can
// hurt the other, and whether the pair shares one wanted level.  With sharing
// off each player keeps their own record, but a partner's crime is still the
// pair's problem.  Set from the co-op page and saved in the INI under "Wii".
extern int8_t CoopFriendlyFire;
extern int8_t CoopSharedWanted;

class CCoop
{
public:
	// --- the session -------------------------------------------------------
	// Forgets everything.  For a new game, a loaded game and shutdown; the peds
	// themselves are the world's to free.
	static void Init(void);
	// Once a frame, after the scripts and before the world is processed.
	static void Update(void);
	// Takes the partner out of the world right now.  They come back by
	// themselves once whatever asked for this is over.
	static void Suspend(const char *why);

	// Whether the shared-view half of the session is on.  The session is on
	// from the moment the menu row is set -- the pads are watched and the join
	// is offered -- but until somebody presses a button on a partner's pad
	// there is one player, and the game is the ordinary game: the follow
	// camera and its own crosshair, the scopes, the drive-bys.  Everything
	// that only exists because the view or the party is shared asks this,
	// never the menu toggle.
	static bool PairActive(void);
	// The first partner's ped (player 2's), or nil when there is none in the
	// world right now.  For the callers that still speak of one partner --
	// the shared camera and the Wiimote speaker; see GetPlayerPed for the
	// general case.
	static CPlayerPed *GetPartner(void);
	// A player's ped by slot: 0 is player 1, 1..3 the partners.  Nil when that
	// player is not in the world.  Slots 1..3 are the co-op slots: they hold
	// a ped only while that partner's controller is there and they have
	// joined, and are nil otherwise.
	static CPlayerPed *GetPlayerPed(int player);
	// 0..3 for a co-op player, -1 for anything else.
	static int GetPlayerIndex(const CEntity *entity);
	// Whether damage from one player to another is switched off.  With friendly
	// fire off, a hit that lands on a player from a player -- or from a car one
	// of them is driving -- costs nothing.  The weapon paths still show their
	// hit feedback on purpose: a glance, the blood.  Only CPed::InflictDamage
	// drops the damage, and every path ends up there.
	static bool FriendlyFireBlocked(CEntity *attacker, CEntity *victim);

	// --- staying together ----------------------------------------------------
	// The limit on how far apart the two players can get on foot.  Given the
	// ground velocity a player ped is about to move with, takes out however
	// much of it would carry them further from the other player than they are
	// allowed to be.  Walking back, or along the edge, is untouched.
	static void LimitSeparation(CPed *ped, CVector2D &moved);

	// --- from the platform layer --------------------------------------------
	// Whether a controller for a partner is connected.  Reported every frame;
	// a partner joins when one has been there a moment and drops out when it
	// has been gone a while.  The partner index is 0 for the first partner,
	// 1 and 2 for the second and third; that is the pad layer's numbering,
	// and slot = index + 1 is the player's.
	static void ReportPartnerPad(int partner, bool present);
	// Where a player's pointer is on the screen, as fractions of it.  Only
	// reported while a pointer is actually driving that player's reticle.
	static void ReportPointer(int player, float x, float y);

	// --- aiming --------------------------------------------------------------
	// Each player aims at their own reticle rather than along the camera.  True
	// while the shared camera is actually the one on screen, in the Standard
	// control method; Classic keeps its lock-on, which never needed the camera.
	static bool UsesReticleAim(void);
	// Works out, once a frame, whether this player's body should face their
	// reticle (and strafe) or the way they are running, and remembers it.
	static bool FacesAim(CPlayerPed *ped, CPad *pad);
	// What FacesAim last decided for this ped.
	static bool IsFacingAim(CPed *ped);
	// Whether this player has anything to aim with right now.
	static bool HasAim(CPed *ped);
	static float GetAimHeading(CPed *ped);
	// The pitch CPed::AimGun wants: negative is up.
	static float GetAimPitch(CPed *ped);
	// The line a shot leaves along: from the muzzle, toward what the reticle is
	// on.  Returns false when the reticle is on nothing in particular, in which
	// case the line is level and the caller may let the engine's own vertical
	// auto-aim (CWeapon::DoDoomAiming) finish the job.
	static bool FindShotVector(CEntity *shooter, float range, const CVector &fireSource, CVector &source, CVector &target);
	// Where to draw a player's reticle, as fractions of the screen.  False when
	// that player has none.  engaged is true while it is on a target.
	static bool GetReticle(int player, float &x, float &y, bool &engaged);

	// --- camera --------------------------------------------------------------
	// Which of the shared camera's framings is in use; see Cam.cpp.  Cycled by
	// player 1's camera button, kept in the INI.
	static int8 ms_nFraming;
	// Which car the shared camera turns with when both players are driving.  False
	// follows player 1's; true follows the partner's.  The camera can only follow
	// one nose, and following player 1's leaves the partner driving half blind, so
	// the partner's camera button hands it over -- the job San Andreas gives its
	// Select button.
	static bool ms_bPartnerFocus;
	enum { NUM_FRAMINGS = 4 };

private:
	static bool ms_bRunning;
};
