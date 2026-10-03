#pragma once

#ifdef AIM_ASSIST

// Aim assist for the Standard (free aim) control method.
//
// In Standard the crosshair sits at a fixed point on screen and shots leave along
// the camera's ray through it (CCamera::Find3rdPersonCamTargetVector), so aiming
// is nothing but steering the camera.  The assist therefore works on the look
// input the mouse camera is about to apply, with no lock-on and no camera mode
// change: the crosshair never leaves the screen and the view never jumps.
//
//  - Slower look: the camera turns at 70 % speed whenever the aim button is held,
//    as in any shooter, so fine aim is easier than at walking-around speed.
//  - Friction: with the crosshair near a target the look speed drops further, so
//    it is easier to settle on and to stay on.
//  - Magnetism: the crosshair is eased toward the target's chest, which also
//    carries it along when the target moves.  It lets go the moment the player
//    pushes away from the target.
//
// It acts only while the aim button or the trigger is held with a gun that can
// aim (the slower look is the aim button's alone).
class CAimAssist
{
public:
	// The "Aim Assist" option; persisted to the INI.
	static int8 bEnabled;

	// True while the assist is holding a target near the crosshair, for the HUD to
	// show it.  Time-based because Process only runs while the mouse camera does.
	static bool IsEngaged(void);

	// Called by CCam::Process_FollowPedWithMouse with the offsets (radians) it is
	// about to add to the camera's Alpha (pitch) and Beta (yaw); bends them in place.
	static void Process(const CVector &source, const CVector &front, const CVector &up, float fov,
	                    float &alphaOffset, float &betaOffset);
};

#endif
