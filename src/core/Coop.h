#pragma once

class CEntity;
class CPad;
class CPed;
class CPlayerPed;
class CVector;
class CVector2D;

// Couch co-op: a second player on the same screen, GTA: San Andreas PS2 style.
// The design, and the reasoning behind every rule below, is in
// COUCH-COOP-DESIGN.md.  This class is the session: when co-op is in charge,
// who the second player is, and where each player is aiming.
//
// Three things it deliberately does NOT do, because the rest of the engine
// relies on them:
//
//   It never changes CWorld::PlayerInFocus.  FindPlayerPed() is player 1 for
//   as long as the game runs, so scripts, the HUD, pickups and the save all go
//   on talking to the player they have always talked to.
//
//   It never runs during a mission.  The session is off for as long as
//   CTheScripts::IsPlayerOnAMission() is true, and the partner is taken out of
//   the world for that time rather than left standing in a scripted scene.
//
//   It never trusts its own pointer to the partner.  The ped lives in
//   CWorld::Players[1].m_pPed as a registered reference, which the engine nils
//   the moment the ped is deleted -- and the engine may delete it, for instance
//   along with a vehicle it was sitting in.  Everything here re-reads that slot
//   and treats nil as "bring them back", so there is nothing to dangle.
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

	// Co-op is in charge this frame: the shared camera is wanted and a partner
	// may be in the world.  False during missions, cutscenes and while player 1
	// is down, whatever the menu toggle says.
	static bool IsRunning(void) { return ms_bRunning; }
	// Player 2's ped, or nil when there is none in the world right now.
	static CPlayerPed *GetPartner(void);
	// 0 for player 1, 1 for the partner, -1 for anything else.
	static int GetPlayerIndex(const CEntity *entity);

	// --- staying together ----------------------------------------------------
	// The limit on how far apart the two players can get on foot.  Given the
	// ground velocity a player ped is about to move with, takes out however
	// much of it would carry them further from the other player than they are
	// allowed to be.  Walking back, or along the edge, is untouched.
	static void LimitSeparation(CPed *ped, CVector2D &moved);

	// --- from the platform layer --------------------------------------------
	// Whether a controller for player 2 is connected.  Reported every frame;
	// the partner joins when one has been there a moment and drops out when it
	// has been gone a while.
	static void ReportPartnerPad(bool present);
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
