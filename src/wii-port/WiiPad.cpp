#include <cmath>
#include <cstdio>

#include <gccore.h>
#include <ogc/consol.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "common.h"
#include "Camera.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Pad.h"
#include "PlayerPed.h"
#include "Timer.h"
#include "WiiPad.h"
#include "WiiPointerAim.h"
#include "WiiSpeaker.h"
#include "WiiTrace.h"
#include "platform.h"
#include "skeleton.h"

CPlayerPed *FindPlayerPed(void);

// Stick handling ported from the BetaPlusPlus Wii port (src/wii/wii/
// WiiPadState.cpp in the reference tree).  The failure modes it works around
// are the ones that only show up on real hardware, so the reasoning is kept
// with the code rather than rediscovered later.

namespace
{

// Every axis in CControllerState is measured against 128, not against the range
// the int16 holding it could carry.  CPad::LookAroundUpDown only starts turning
// the camera past 85 and treats 128 as the edge of the gate,
// CPad::GetSteeringLeftRight subtracts 35 and rescales by 128/(128 - 35),
// CPad::GetAnalogueUpDown weighs the stick against a D-pad worth 127, and every
// other joystick backend writes value*128*sensitivity (the XInput path in
// Pad.cpp, sdl2.cpp, glfw.cpp).  Filling the int16 range instead puts a barely
// moved stick two orders of magnitude past hard over, so all of those thresholds
// are already saturated and the first nudge reads as a full speed turn.
constexpr float kAxisFullScale = 128.0f;

// PAD_ScanPads() calls PAD_Read() and nothing else, so the sticks arrive
// unclamped: PAD_Stick* return the raw sample minus the calibrated origin, with
// neither the deadzone nor the gate PAD_Clamp() would have imposed.  Where that
// gate is written down is the useful part, because it is not a circle: PAD_Clamp
// subtracts a 15 count deadzone per axis and then clamps into an OCTAGON, with
// vertices at (72, 0) and (40, 40) for the main stick and (59, 0) and (31, 31)
// for the C-stick.  Adding the deadzone back gives the same gate in the raw
// counts this backend actually sees -- 87 out to a cardinal notch, 55 on each
// axis out to a diagonal one.
//
// Those are the limits every pad is guaranteed to reach, which is why they are
// what to normalise against: the ~100 counts the s8 could carry is a number many
// pads never report, so dividing by it leaves a stick held hard against the gate
// still reading as only two thirds deflected.
constexpr float kGameCubeStickRange = 87.0f;
constexpr float kGameCubeStickCorner = 55.0f;
constexpr float kGameCubeSubStickRange = 74.0f;
constexpr float kGameCubeSubStickCorner = 46.0f;

// A deadzone of one or more would divide the rescale in applyDeadzone by zero,
// and nothing validates the settings on the way in.
constexpr float kMaximumDeadzone = 0.9f;

constexpr float kDegreesToRadians = 3.14159265358979323846f/180.0f;

// --- the pointer as a camera ------------------------------------------------
// The hard part of a pointer on this engine: IR is ABSOLUTE and BOUNDED, it says
// where on the screen the player is aiming, while the camera is RELATIVE and
// UNBOUNDED, it wants how far to turn and can turn forever.  Differencing
// successive pointer positions is the obvious translation and it is wrong in a
// specific way: the total camera travel available becomes one screen width, so
// aiming into a corner and holding swings the view until the pointer clamps and
// then stops.  There is no way to keep turning without sweeping back and forth.
//
// So in game the offset from the centre of the screen is read as a RATE instead:
// direction and speed, not distance.  Hold the Wiimote up and right and the
// camera keeps going up and right, faster the further out it points.  In a MENU
// the pointer stays absolute, because there it really is a cursor and aiming at
// an option has to put the cursor on that option.
//
// Neutral is the centre of the screen, always.  Latching it to wherever the
// player happened to be aiming sounds friendlier and is not: close a menu with
// the pointer in a corner and the whole mapping is half a screen out, with
// nothing on screen to say that it happened or how to undo it.
//
// All three of these are fractions of half the screen HEIGHT, including the
// horizontal one -- see irPointerRate for why not each axis' own extent.
//
//   deadzone    how far off centre the player can aim before the camera moves.
//               Has to cover ordinary hand shake plus the sensor's own jitter or
//               the view drifts while they hold still, and it is also what keeps
//               a centred pointer from overriding the sticks.
//   saturation  where the turn rate reaches its maximum.  Deliberately short of
//               1.0: the pointer gets unreliable near the edge of the sensor's
//               field, and having to aim there to turn quickly is what makes a
//               rate camera feel like it is fighting the player.
//   curve       how much of the response is linear rather than quadratic.  The
//               quadratic part is what keeps small offsets slow enough to aim
//               with; the linear part stops the first third from doing nothing.
constexpr float kPointerDeadzone = 0.13f;
constexpr float kPointerSaturation = 0.75f;
constexpr float kPointerCurveLinear = 0.45f;

// Turn rate at full deflection, in the units GetMouseX and GetMouseY are read
// in, PER SECOND.  Per second and not per frame because this port's frame rate
// moves with the scene, and a per frame constant makes the camera turn faster in
// an empty street than in traffic, which reads as the sensitivity changing by
// itself.
//
// Cam.cpp turns a horizontal delta into 2.5*x*m_fMouseAccelHorzntl radians of
// yaw, so at the default sensitivity of 0.0025 one unit is 0.00625 rad and this
// works out to about 143 degrees per second at 60Hz.  The options screen's mouse
// sensitivity slider scales m_fMouseAccelHorzntl, and therefore scales the
// pointer with it, which is the control to reach for rather than this number:
// Frontend.cpp clamps it between 1/3200 and 1/200, so the pointer can be tuned
// across a factor of sixteen without touching the source.
constexpr float kPointerRatePerSec = 400.0f;

// The same delta pitches further than it yaws: Cam.cpp scales the vertical one
// by 4.0*m_fMouseAccelVertical against 2.5*m_fMouseAccelHorzntl for the
// horizontal, and m_fMouseAccelVertical is m_fMouseAccelHorzntl + 0.0005, so at
// the default settings pitch comes out about twice as fast as yaw.  This takes
// that back out and leaves the pitch a little slower than the yaw instead, the
// way the stick path's own 0.6 factor in Cam.cpp does.
constexpr float kPointerPitchScale = 0.32f;

// The pointer stops being tracked the moment it leaves the sensor bar's field,
// which is exactly what happens at the END of a long turn: the remote is still
// held out to the side and the player still wants to keep turning.  Freezing
// there caps a single turn at the sensor's field of view and forces them to saw
// the remote back and forth, so the last rate is held instead.
//
// Not held forever, though.  A remote set down, or pointed at the floor, looks
// identical from here, and a camera that keeps spinning until someone picks it
// back up reads as a hang rather than a feature.  This is long enough to finish
// any turn a player is actually in the middle of.
constexpr float kPointerHoldSeconds = 2.0f;

// Frame time clamp for the rate above.  A dt of zero (two polls inside one
// timebase tick) would freeze the camera and a huge one -- the first frame after
// a streaming stall -- would fling it, so the rate is only integrated over
// plausible frame times.
constexpr float kMinPointerDt = 1.0f/240.0f;
constexpr float kMaxPointerDt = 1.0f/15.0f;

// --- the pointer as a crosshair ---------------------------------------------
// Standard aiming draws a crosshair at a fixed point and traces the shot through
// it, so aiming is steering the camera, and with the rate camera above that means
// pushing the pointer off centre and waiting for the view to get there.  With a
// gun out the pointer can do what it is best at: put the crosshair where it
// points.  The crosshair is two numbers on CCamera that the HUD draws at and the
// shot is traced through (CCamera::Find3rdPersonCrosshairRay), so setting them
// moves both, and the camera only has to turn when the player aims past the edge
// of a box around the middle of the screen, the way Metroid Prime 3 does it.
//
//   box         where the crosshair can go, as fractions of the screen.  Past it
//               the crosshair sits on the edge and the camera turns instead.  Three
//               sizes, picked on the controls page: a small one for players who
//               want the camera moving sooner, a large one for those who want to
//               aim with the pointer almost everywhere.
//   saturation  how far past the box, in half screen heights as above, the pointer
//               has to be for the full turn rate.
//   smoothing   the pointer jitters, and that would shake the crosshair, so it
//               follows through a low pass whose time constant shrinks with how
//               far it has to travel: heavy while the hand is nearly still,
//               almost none in a fast sweep.
//               tau = kAimSmoothTau/(1 + kAimSmoothGain*error)
constexpr float kAimDefaultX = 0.53f;	// CCamera::Init's resting crosshair
constexpr float kAimDefaultY = 0.4f;
struct AimBox
{
	float left, right, top, bottom;
};
constexpr AimBox kAimBoxes[] = {
	{ 0.38f, 0.64f, 0.28f, 0.54f },
	{ 0.30f, 0.72f, 0.20f, 0.62f },
	{ 0.20f, 0.82f, 0.12f, 0.70f },
};
constexpr float kAimSaturation = 0.28f;
constexpr float kAimSmoothTau = 0.09f;
constexpr float kAimSmoothGain = 60.0f;

// --- what one scan leaves behind for the rest of the frame -------------------
// WiiPadScan fills these and everything below reads them, which is the whole
// reason it is a separate entry point; see WiiPad.h.
uint32 s_connectedGameCubePads;
float s_pointerDt = 1.0f/60.0f;   // seeded so the first frame is not a special case
u64 s_pointerLastTime;

// The last rate the pointer asked for, kept so it can go on being applied while
// tracking is lost.  Stored per second rather than per frame: it outlives the
// frame it was measured in, and the frames it is replayed over are not the same
// length as that one.
float s_heldRateX;
float s_heldRateY;
float s_heldSeconds;

// Where the crosshair currently is, after smoothing, while the pointer owns it.
float s_aimX;
float s_aimY;
bool s_aimActive;

int16
toAxis(float value, float sensitivity)
{
	if(value > 1.0f)
		value = 1.0f;
	if(value < -1.0f)
		value = -1.0f;
	return (int16)(value*kAxisFullScale*sensitivity);
}

void
setButton(int16 &field, bool down)
{
	if(down)
		field = 255;
}

float
clampDeadzone(float deadzone)
{
	// Written as a failed greater-than so a NaN out of the settings lands here
	// instead of propagating into every axis.
	if(!(deadzone > 0.0f))
		return 0.0f;
	if(deadzone > kMaximumDeadzone)
		return kMaximumDeadzone;
	return deadzone;
}

// Deadzone and rescale for a stick as a whole rather than for each of its axes
// on its own.  A per axis deadzone lets an axis through the moment it clears the
// threshold by itself, so a stick pushed nearly horizontally still leaks its
// small vertical component and the camera climbs while the player believes they
// are only panning.  Working from the magnitude also leaves the direction alone:
// only the length is rescaled, so a diagonal stays a diagonal instead of being
// bent towards the nearer axis.
void
applyDeadzone(float &x, float &y, float deadzone)
{
	const float magnitude = std::sqrt(x*x + y*y);
	if(magnitude <= deadzone || magnitude <= 0.0f){
		x = 0.0f;
		y = 0.0f;
		return;
	}

	// Movement starts at zero just outside the deadzone instead of jumping
	// straight to whatever fraction of full deflection the deadzone covers.
	float length = (magnitude - deadzone)/(1.0f - deadzone);
	if(length > 1.0f)
		length = 1.0f;
	const float scale = length/magnitude;
	x *= scale;
	y *= scale;
}

// Rescales a raw GameCube reading so that every point on the gate reaches full
// deflection, a diagonal notch as much as a cardinal one, with the direction
// left alone.  Dividing both axes by the cardinal reach instead treats the gate
// as a square: a diagonal notch only carries 55 counts per axis against the 87 a
// cardinal one carries, so the same physical travel reads about a fifth shorter
// once the stick is held off axis, and the camera turns visibly slower on the
// diagonals than it does straight up or sideways.
//
// The gate's radius in the direction being pushed needs no trigonometry.  The
// octagon's boundary, in the sector where one axis dominates, is
// corner*major + (range - corner)*minor = corner*range, so evaluating that left
// hand side against corner*range IS the deflection as a fraction of the gate.
void
normalizeGameCubeStick(float &x, float &y, float range, float corner)
{
	const float magnitude = std::sqrt(x*x + y*y);
	if(magnitude <= 0.0f)
		return;

	const float absX = std::fabs(x);
	const float absY = std::fabs(y);
	const float deflection = (corner*Max(absX, absY) +
		(range - corner)*Min(absX, absY))/(corner*range);

	const float scale = deflection/magnitude;
	x *= scale;
	y *= scale;
}

// libogc reports an expansion stick in polar form derived from a calibration
// block read during the expansion handshake, and that block is what goes wrong
// in practice: on a third party Classic Controller, or while the handshake is
// still in flight, centre/min/max come back as zeros and the polar pair is then
// either stuck at zero (the stick does nothing) or wildly out of range (the
// player walks in one direction forever).  So the polar values are used when
// they are sane and the raw position measured against the calibrated centre is
// the fallback.  The fallback also produces zero for a genuinely centred stick,
// which is why no "is the calibration broken" flag is needed anywhere.
void
readJoystick(const joystick_t &joystick, float &outX, float &outY)
{
	outX = 0.0f;
	outY = 0.0f;

	const float magnitude = joystick.mag > 1.0f ? 1.0f : joystick.mag;
	if(magnitude > 0.0f && std::isfinite(magnitude) && std::isfinite(joystick.ang)){
		const float radians = joystick.ang*kDegreesToRadians;
		outX = std::sin(radians)*magnitude;
		outY = std::cos(radians)*magnitude;
		return;
	}

	// A centre of zero means no calibration was ever read.  There is nothing to
	// measure against, so report centred rather than invent a direction.
	const int centerX = joystick.center.x;
	const int centerY = joystick.center.y;
	if(centerX == 0 && centerY == 0)
		return;

	// Half the calibrated travel, or the nominal counts when the calibration
	// block is obviously unusable.
	const float rangeX = joystick.max.x > joystick.min.x ?
		(joystick.max.x - joystick.min.x)*0.5f : 100.0f;
	const float rangeY = joystick.max.y > joystick.min.y ?
		(joystick.max.y - joystick.min.y)*0.5f : 100.0f;
	outX = ((float)joystick.pos.x - (float)centerX)/rangeX;
	outY = ((float)joystick.pos.y - (float)centerY)/rangeY;

	if(outX > 1.0f)
		outX = 1.0f;
	else if(outX < -1.0f)
		outX = -1.0f;
	if(outY > 1.0f)
		outY = 1.0f;
	else if(outY < -1.0f)
		outY = -1.0f;
}

struct StickAccumulator
{
	float leftX;
	float leftY;
	float rightX;
	float rightY;
};

// The joystick tuning the rest of the game already carries, read once per
// capture so every device merged into this pad is shaped the same way and the
// numbers live where the other backends already look for them.
struct StickSettings
{
	float leftDeadzone;
	float rightDeadzone;
	float leftSensitivityX;
	float leftSensitivityY;
	float rightSensitivityX;
	float rightSensitivityY;
};

StickSettings
currentStickSettings(void)
{
	StickSettings settings;
	settings.leftDeadzone = clampDeadzone(ControlsManager.m_lStickDeadzone);
	settings.rightDeadzone = clampDeadzone(ControlsManager.m_rStickDeadzone);
	settings.leftSensitivityX = ControlsManager.m_lStickSensX;
	settings.leftSensitivityY = ControlsManager.m_lStickSensY;
	settings.rightSensitivityX = ControlsManager.m_rStickSensX;
	settings.rightSensitivityY = ControlsManager.m_rStickSensY;
	return settings;
}

// The deadzone is applied per device, before the merge, because each one has its
// own idle noise: a Classic Controller resting off centre must not be able to
// push a GameCube pad that is genuinely centred past the threshold.
void
addStick(float x, float y, float deadzone, float &outX, float &outY)
{
	applyDeadzone(x, y, deadzone);
	outX += x;
	outY += y;
}

// PAD_ScanPads() returns the mask of channels whose status came back without
// error, so it is a real connected test.  Guessing presence from stick
// deflection instead lets an idle pad resting a couple of counts off centre
// claim every frame.
bool
playerInVehicle(void)
{
	CPlayerPed *ped = FindPlayerPed();
	return ped != nil && ped->bInVehicle;
}

bool
captureGameCube(int channel, uint32 connectedMask, CControllerState &state,
	StickAccumulator &sticks, const StickSettings &settings)
{
	if((connectedMask & (1 << channel)) == 0)
		return false;

	const u16 buttons = PAD_ButtonsHeld(channel);
	const bool inCar = playerInVehicle();

	// Face buttons: A=Cross (accelerate / sprint), B=Circle (Mode 0 GetWeapon =
	// shoot / brake).  Keep that so B is always fire on foot.
	setButton(state.Cross, buttons & PAD_BUTTON_A);
	setButton(state.Circle, buttons & PAD_BUTTON_B);
	setButton(state.Square, buttons & PAD_BUTTON_X);
	setButton(state.Triangle, buttons & PAD_BUTTON_Y);
	setButton(state.Start, buttons & PAD_BUTTON_START);

	// Mode 0 GetTarget / auto-aim reads RightShoulder1.  L owns that now; R is
	// free of aim so it does not double up.  Horn (LeftShock) moves to Z so L
	// is not shared with radio/aim again.
	setButton(state.RightShoulder1, buttons & PAD_TRIGGER_L);
	setButton(state.LeftShock, buttons & PAD_TRIGGER_Z);

	if(inCar){
		// Only in a vehicle: D-Pad is remapped away from steering.
		// Radio = ChangeStationJustDown -> LeftShoulder1 <- D-Pad Up.
		// Look L/R = LeftShoulder2 / RightShoulder2 <- D-Pad Left / Right.
		setButton(state.LeftShoulder1, buttons & PAD_BUTTON_UP);
		setButton(state.LeftShoulder2, buttons & PAD_BUTTON_LEFT);
		setButton(state.RightShoulder2, buttons & PAD_BUTTON_RIGHT);
	}else{
		// On foot: leave state.DPad* unset so the pad does not walk / steer.
		// CycleWeaponLeft/Right read L2/R2 -- bind those to D-Pad Left / Right.
		setButton(state.LeftShoulder2, buttons & PAD_BUTTON_LEFT);
		setButton(state.RightShoulder2, buttons & PAD_BUTTON_RIGHT);
		// CollectPickupJustDown Mode 0 still wants LeftShoulder1; R fills it on foot.
		setButton(state.LeftShoulder1, buttons & PAD_TRIGGER_R);
	}

	// PAD_Stick* report raw counts offset from the calibrated origin, so the gate
	// is normalised away before anything else looks at them.  Their Y grows
	// upwards while the game's grows down the screen; the octagon is symmetric
	// about both axes, so flipping it first costs nothing.
	float x = PAD_StickX(channel);
	float y = -PAD_StickY(channel);
	normalizeGameCubeStick(x, y, kGameCubeStickRange, kGameCubeStickCorner);
	addStick(x, y, settings.leftDeadzone, sticks.leftX, sticks.leftY);

	x = PAD_SubStickX(channel);
	y = -PAD_SubStickY(channel);
	normalizeGameCubeStick(x, y, kGameCubeSubStickRange, kGameCubeSubStickCorner);
	addStick(x, y, settings.rightDeadzone, sticks.rightX, sticks.rightY);
	return true;
}

// WPAD_Probe is the supported "what is plugged into this channel" query and is
// consulted in preference to exp.type, which only describes the last data report
// that arrived: around a hot plug, or while a handshake is still in flight, it
// can read NONE with an expansion very much attached.  Neither query gets to veto
// the other, so a probe that fails for a single frame cannot silence the pad.
u32
probeExpansion(int channel, const WPADData &data)
{
	u32 expansion = WPAD_EXP_NONE;
	if(WPAD_Probe(channel, &expansion) != WPAD_ERR_NONE)
		expansion = WPAD_EXP_NONE;
	if(expansion == WPAD_EXP_NONE)
		expansion = (u32)data.exp.type;
	return expansion;
}

void
captureClassic(const WPADData &data, CControllerState &state,
	StickAccumulator &sticks, const StickSettings &settings)
{
	const u32 buttons = data.btns_h;
	const bool inCar = playerInVehicle();

	setButton(state.Cross, buttons & WPAD_CLASSIC_BUTTON_B);
	setButton(state.Circle, buttons & WPAD_CLASSIC_BUTTON_A);
	setButton(state.Square, buttons & WPAD_CLASSIC_BUTTON_Y);
	setButton(state.Triangle, buttons & WPAD_CLASSIC_BUTTON_X);
	setButton(state.LeftShoulder1, buttons & WPAD_CLASSIC_BUTTON_FULL_L);
	setButton(state.RightShoulder1, buttons & WPAD_CLASSIC_BUTTON_FULL_R);
	setButton(state.LeftShoulder2, buttons & WPAD_CLASSIC_BUTTON_ZL);
	setButton(state.RightShoulder2, buttons & WPAD_CLASSIC_BUTTON_ZR);
	// Mode 0 GetHorn reads LeftShock (PS2 L3).  D-Pad Up is the horn; do not
	// also write state.DPadUp or steering will fire with the horn.
	setButton(state.LeftShock, buttons & WPAD_CLASSIC_BUTTON_UP);
	setButton(state.Start, buttons & WPAD_CLASSIC_BUTTON_PLUS);
	setButton(state.Select, buttons & WPAD_CLASSIC_BUTTON_MINUS);

	if(inCar){
		// In a vehicle: D-Pad L/R look (GetLookLeft/Right) without dropping ZL/ZR.
		setButton(state.LeftShoulder2, (buttons & WPAD_CLASSIC_BUTTON_ZL) ||
			(buttons & WPAD_CLASSIC_BUTTON_LEFT));
		setButton(state.RightShoulder2, (buttons & WPAD_CLASSIC_BUTTON_ZR) ||
			(buttons & WPAD_CLASSIC_BUTTON_RIGHT));
	}else{
		// On foot: D-Pad L/R change weapons; other D-Pad bits stay unmapped.
		setButton(state.LeftShoulder2, (buttons & WPAD_CLASSIC_BUTTON_ZL) ||
			(buttons & WPAD_CLASSIC_BUTTON_LEFT));
		setButton(state.RightShoulder2, (buttons & WPAD_CLASSIC_BUTTON_ZR) ||
			(buttons & WPAD_CLASSIC_BUTTON_RIGHT));
		setButton(state.DPadDown, buttons & WPAD_CLASSIC_BUTTON_DOWN);
	}

	// readJoystick already normalises to -1..1 with +Y upwards, so only the sign
	// of Y has to be turned around to match the screen.
	const classic_ctrl_t &classic = data.exp.classic;
	float x, y;
	readJoystick(classic.ljs, x, y);
	addStick(x, -y, settings.leftDeadzone, sticks.leftX, sticks.leftY);
	readJoystick(classic.rjs, x, y);
	addStick(x, -y, settings.rightDeadzone, sticks.rightX, sticks.rightY);
}

// The Wiimote itself, and the Nunchuk when one is attached.  Only the forward
// grip is supported -- the remote pointed at the screen, D-pad under the thumb,
// B trigger under the index finger -- because that is the grip the pointer needs
// and the pointer is what stands in for the right stick.  Held sideways the
// D-pad would be rotated a quarter turn and the pointer unusable, which is a
// different mapping rather than the same one with a caveat.
void
captureWiimote(const WPADData &data, u32 expansion, CControllerState &state,
	StickAccumulator &sticks, const StickSettings &settings)
{
	const u32 buttons = data.btns_h;
	const bool hasNunchuk = expansion == WPAD_EXP_NUNCHUK;
	const bool inCar = playerInVehicle();
	const bool dpadLeft = (buttons & WPAD_BUTTON_LEFT) != 0;
	const bool dpadRight = (buttons & WPAD_BUTTON_RIGHT) != 0;
	const bool dpadDown = (buttons & WPAD_BUTTON_DOWN) != 0;

	// With a Nunchuk the stick walks and steers, which frees the D-pad for what the
	// L2 and R2 buttons do on other pads and a Wiimote has no way to press: cycle
	// weapons on foot, and in a vehicle look left and right (down looks behind,
	// which is both at once).  The GameCube pad is mapped the same way.  Menus
	// still get the D-pad as a D-pad.
	const bool dpadIsShoulders = hasNunchuk && !FrontEndMenuManager.m_bMenuActive;
	const bool lookingOut = inCar && dpadIsShoulders && (dpadLeft || dpadRight);

	// B is the trigger, Circle, on foot.  In a vehicle it brakes and reverses, the
	// way Mario Kart has it, because the 1 button (which still does too) is a long
	// stretch for the thumb that is already on A.  The exception is looking out of
	// a side window, where B fires the drive-by instead.
	const bool b = (buttons & WPAD_BUTTON_B) != 0;
	setButton(state.Circle, b && (!inCar || lookingOut));
	setButton(state.Cross, buttons & WPAD_BUTTON_A);
	setButton(state.Square, (buttons & WPAD_BUTTON_1) || (inCar && b && !lookingOut));

	// One event-log line per press of brake/reverse in a vehicle, so a report that
	// "reverse does nothing" can be settled from debug.log: the line says the pad layer
	// saw the press and sent Square, which leaves the car code as the only suspect.
	static bool s_brakeLogged;
	const bool braking = inCar && ((buttons & WPAD_BUTTON_1) || (b && !lookingOut));
	if(braking != s_brakeLogged){
		s_brakeLogged = braking;
		if(braking)
			WiiTraceReport("WII pad: vehicle brake/reverse pressed (%s)\n",
				(buttons & WPAD_BUTTON_1) ? "1" : "B");
	}
	setButton(state.Triangle, buttons & WPAD_BUTTON_2);
	setButton(state.Start, buttons & WPAD_BUTTON_PLUS);
	setButton(state.Select, buttons & WPAD_BUTTON_MINUS);

	if(dpadIsShoulders){
		setButton(state.LeftShoulder2, dpadLeft || (inCar && dpadDown));
		setButton(state.RightShoulder2, dpadRight || (inCar && dpadDown));
	}else{
		// CPad::GetPedWalkUpDown and GetSteeringUpDown weigh the D-pad against the
		// left stick at 255/2, so on a bare Wiimote the D-pad alone walks and drives
		// without any synthetic stick behind it.
		setButton(state.DPadUp, buttons & WPAD_BUTTON_UP);
		setButton(state.DPadDown, buttons & WPAD_BUTTON_DOWN);
		setButton(state.DPadLeft, buttons & WPAD_BUTTON_LEFT);
		setButton(state.DPadRight, buttons & WPAD_BUTTON_RIGHT);
	}

	if(!hasNunchuk)
		return;

	// libogc reports the expansion's buttons twice: merged into the Wiimote mask
	// (WPAD_NUNCHUK_BUTTON_* are the low bits shifted up by 16) and in the
	// expansion struct's own byte.  Either is normally enough; taking both costs
	// one OR and removes a whole class of "works on my Wiimote" difference.
	const nunchuk_t &nunchuk = data.exp.nunchuk;
	setButton(state.RightShoulder1, (buttons & WPAD_NUNCHUK_BUTTON_Z) ||
		(nunchuk.btns_held & NUNCHUK_BUTTON_Z));
	setButton(state.LeftShoulder1, (buttons & WPAD_NUNCHUK_BUTTON_C) ||
		(nunchuk.btns_held & NUNCHUK_BUTTON_C));
	// Same LeftShock fill as GameCube: C already drives LeftShoulder1 and also
	// stands in for L3 so GetHorn works in Mode 0.
	setButton(state.LeftShock, (buttons & WPAD_NUNCHUK_BUTTON_C) ||
		(nunchuk.btns_held & NUNCHUK_BUTTON_C));

	float x, y;
	readJoystick(nunchuk.js, x, y);
	addStick(x, -y, settings.leftDeadzone, sticks.leftX, sticks.leftY);
}

// Offset from the centre of the screen turned into a turn rate, in the units
// CPad::GetMouseX and GetMouseY are read in, PER SECOND -- the caller decides
// how long to apply it for, which is what lets the rate outlive the frame it was
// measured in when tracking drops out.  Returns false when the pointer is inside
// the dead zone, which is what leaves the sticks in charge of the camera while
// the player is not aiming anywhere in particular.
bool
irPointerRate(const WPADData &data, float &outX, float &outY)
{
	const float width = (float)RsGlobal.maximumWidth;
	const float height = (float)RsGlobal.maximumHeight;
	if(width <= 0.0f || height <= 0.0f)
		return false;

	// Normalise BOTH axes by half the HEIGHT, not by each axis' own half extent.
	// Dividing x by w/2 and y by h/2 stretches the vector horizontally, so a
	// pointer 45 degrees up and right would not turn the camera 45 degrees up and
	// right, and matching the direction at every angle is the whole point.  The
	// cost is that the maximum yaw rate arrives about three quarters of the way
	// to the left and right edges rather than at the edge itself, which given
	// kPointerSaturation is no cost at all.
	const float half = height*0.5f;
	const float unitX = (data.ir.x - width*0.5f)/half;
	const float unitY = (data.ir.y - height*0.5f)/half;

	// Radial dead zone, not a per axis one.  A square dead zone lets a pointer
	// inside it on X but outside on Y turn the camera straight up, which is the
	// "near the middle it only moves vertically" complaint.
	const float magnitude = std::sqrt(unitX*unitX + unitY*unitY);
	if(magnitude <= kPointerDeadzone)
		return false;

	float t = (magnitude - kPointerDeadzone)/(kPointerSaturation - kPointerDeadzone);
	if(t > 1.0f)
		t = 1.0f;
	const float curve = t*(kPointerCurveLinear + (1.0f - kPointerCurveLinear)*t);

	// The unit direction comes from the raw vector, so only the SPEED goes
	// through the curve.  Curving each axis on its own would bend diagonals
	// towards the nearer axis, the same directional error the normalisation
	// above avoids.
	const float rate = kPointerRatePerSec*curve;
	outX = (unitX/magnitude)*rate;
	outY = (unitY/magnitude)*rate*kPointerPitchScale;
	return true;
}

// Ends any hold in progress, so a rate that was being replayed while tracking
// was lost cannot survive into whatever the pointer does next.
void
stopPointerHold(void)
{
	s_heldRateX = 0.0f;
	s_heldRateY = 0.0f;
	s_heldSeconds = kPointerHoldSeconds;
}

// Whether the pointer should be moving the crosshair this frame: a gun out whose
// crosshair the HUD shows (the same test as Hud.cpp), in the Standard method, on
// foot, and the option on.  Everything else keeps the fixed crosshair and the
// rate camera.
bool
pointerAimWanted(void)
{
	if(!WiiPointerAimEnabled || !CCamera::m_bUseMouse3rdPerson)
		return false;
	CPlayerPed *player = FindPlayerPed();
	if(player == nullptr || player->bInVehicle ||
	   player->m_nPedState == PED_ENTER_CAR || player->m_nPedState == PED_CARJACK)
		return false;
	if(!TheCamera.Cams[TheCamera.ActiveCam].Using3rdPersonMouseCam())
		return false;
	const eWeaponType weapon = player->GetWeapon()->m_eWeaponType;
	return (weapon >= WEAPONTYPE_COLT45 && weapon <= WEAPONTYPE_RUGER) ||
		weapon == WEAPONTYPE_M60 || weapon == WEAPONTYPE_MINIGUN || weapon == WEAPONTYPE_FLAMETHROWER;
}

// Gives the crosshair back to the game.
void
releaseCrosshair(void)
{
	s_aimActive = false;
	CCamera::m_f3rdPersonCHairMultX = kAimDefaultX;
	CCamera::m_f3rdPersonCHairMultY = kAimDefaultY;
}

// Moves the crosshair toward a target position through the jitter filter.  It
// starts on the target when the pointer first takes it over, rather than gliding
// there from the resting position.
void
steerCrosshair(float targetX, float targetY)
{
	if(!s_aimActive){
		s_aimX = targetX;
		s_aimY = targetY;
		s_aimActive = true;
	}else{
		const float errorX = targetX - s_aimX;
		const float errorY = targetY - s_aimY;
		const float error = std::sqrt(errorX*errorX + errorY*errorY);
		const float tau = kAimSmoothTau/(1.0f + kAimSmoothGain*error);
		const float follow = 1.0f - std::exp(-s_pointerDt/tau);
		s_aimX += errorX*follow;
		s_aimY += errorY*follow;
	}
	CCamera::m_f3rdPersonCHairMultX = s_aimX;
	CCamera::m_f3rdPersonCHairMultY = s_aimY;
}

// The pointer's two jobs while it owns the crosshair.  Where the crosshair goes is
// the pointer clamped into the box, and how fast the camera turns is how far the
// pointer is past it, in the same units and with the same curve as irPointerRate
// so the two feel like one control.  Returns false inside the box, which is what
// leaves the camera still while the player is only aiming.
bool
irAimRate(const WPADData &data, float &outCrosshairX, float &outCrosshairY,
	float &outX, float &outY)
{
	const float width = (float)RsGlobal.maximumWidth;
	const float height = (float)RsGlobal.maximumHeight;
	if(width <= 0.0f || height <= 0.0f)
		return false;

	int size = WiiPointerBox;
	if(size < 0 || size >= (int)(sizeof(kAimBoxes)/sizeof(kAimBoxes[0])))
		size = 1;
	const AimBox &box = kAimBoxes[size];

	const float pointerX = data.ir.x/width;
	const float pointerY = data.ir.y/height;
	const float boxX = pointerX < box.left ? box.left :
		(pointerX > box.right ? box.right : pointerX);
	const float boxY = pointerY < box.top ? box.top :
		(pointerY > box.bottom ? box.bottom : pointerY);
	outCrosshairX = boxX;
	outCrosshairY = boxY;

	const float half = height*0.5f;
	const float overX = (pointerX - boxX)*width/half;
	const float overY = (pointerY - boxY)*height/half;
	const float magnitude = std::sqrt(overX*overX + overY*overY);
	if(magnitude <= 0.0f)
		return false;

	float t = magnitude/kAimSaturation;
	if(t > 1.0f)
		t = 1.0f;
	const float curve = t*(kPointerCurveLinear + (1.0f - kPointerCurveLinear)*t);
	const float rate = kPointerRatePerSec*curve;
	outX = (overX/magnitude)*rate;
	outY = (overY/magnitude)*rate*kPointerPitchScale;
	return true;
}

} // namespace

int8_t WiiPointerAimEnabled = 1;
int8_t WiiPointerBox = 1;

void
WiiPadInitialise(int pointerWidth, int pointerHeight)
{
	PAD_Init();
	WPAD_Init();

	// wiiuse picks the report mode itself, from three state flags rather than
	// from the format asked for here: an attached expansion is what adds the
	// expansion bytes (report 0x34 without acceleration, 0x35 with it, 0x37 with
	// the pointer as well), so a Classic Controller would have reported its
	// sticks under any of these.  What this call does decide is the pointer,
	// which arrives only in the IR modes and which nothing else can turn on.
	WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);

	// ir.x and ir.y are the only pair of the three coordinate spaces in ir_t
	// expressed in a resolution of our choosing, and this is where that
	// resolution is set.  Without it they stay in wiiuse's default space and
	// every offset measured against the centre of the screen is wrong.
	WPAD_SetVRes(WPAD_CHAN_ALL, (u32)pointerWidth, (u32)pointerHeight);
	WiiPadApplyControlDefaults();
}

void
WiiPadApplyControlDefaults(void)
{
	// Control method (Standard/Classic) is deliberately NOT forced here: it is
	// owned by the normal settings load/save so the player's choice persists.
	// This only re-asserts the Wii input tuning (called at boot and again after
	// the INI load so the tuned values win over generic ones).
	// PC gta_vc.set often stores a 0.3 deadzone for XInput.  Classic Zelda walk reads
	// stick magnitude directly; with our gate normalisation that deadzone leaves barely
	// any deflection and movement feels like a nudge of the stick.
	ControlsManager.m_lStickDeadzone = 0.12f;
	ControlsManager.m_lStickSensX = 1.15f;
	ControlsManager.m_lStickSensY = 1.15f;
}

void
WiiPadScan(void)
{
	s_connectedGameCubePads = (uint32)PAD_ScanPads();
	WPAD_ScanPads();
	WiiSpeakerService();
	WiiTraceService();

	// HOME is what a Wii player reaches for to leave a game, and it is not part
	// of any CControllerState, so it is handled here rather than mapped.  Both
	// masks are tested together because btns_d carries the Classic Controller's
	// buttons in its top half, so one test covers the remote and the pad on it.
	//
	// With a game running it asks first, through the frontend's own "quit game?"
	// screen, because quitting raises the same RsGlobal.quit the power and reset
	// buttons raise and throws away everything since the last save.  It quits
	// outright from the title screens (nothing to lose), from that screen itself,
	// and on a second press while the first is still waiting to be shown.
	if(WPAD_ButtonsDown(WPAD_CHAN_0) & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)){
		const bool onQuitScreen = FrontEndMenuManager.m_bMenuActive &&
			FrontEndMenuManager.m_nCurrScreen == MENUPAGE_EXIT;
		if(FrontEndMenuManager.m_bGameNotLoaded || FrontEndMenuManager.m_bQuitPromptRequested || onQuitScreen){
			WiiTraceReport("WII pad: HOME pressed, quitting\n");
			HandleExit();
		}else{
			WiiTraceReport("WII pad: HOME pressed, asking\n");
			FrontEndMenuManager.RequestQuitPrompt();
		}
	}

	// Frame time for the pointer's rate camera.  gettime() is the timebase, which
	// is monotonic and always alive here, unlike CTimer, which stops with the
	// game and would freeze the pointer along with it in any paused state.
	const u64 now = gettime();
	if(s_pointerLastTime != 0){
		const float dt = (float)diff_usec(s_pointerLastTime, now)*1e-6f;
		s_pointerDt = dt < kMinPointerDt ? kMinPointerDt :
			(dt > kMaxPointerDt ? kMaxPointerDt : dt);
	}
	s_pointerLastTime = now;
}

void
WiiPadCapture(int padID, CControllerState &state)
{
	const StickSettings settings = currentStickSettings();

	StickAccumulator sticks = { 0.0f, 0.0f, 0.0f, 0.0f };
	// A live GameCube pad owns this slot alone.  Merging Wiimote / Classic on
	// top of it was what made IR and remote buttons steal camera and actions
	// while a GC controller was already plugged in.
	if(!captureGameCube(padID, s_connectedGameCubePads, state, sticks, settings)){
		WPADData *data = WPAD_Data(padID);
		if(data != nullptr){
			const u32 expansion = probeExpansion(padID, *data);
			if(expansion == WPAD_EXP_CLASSIC)
				captureClassic(*data, state, sticks, settings);
			else
				captureWiimote(*data, expansion, state, sticks, settings);
		}
	}

	state.LeftStickX = toAxis(sticks.leftX, settings.leftSensitivityX);
	state.LeftStickY = toAxis(sticks.leftY, settings.leftSensitivityY);
	state.RightStickX = toAxis(sticks.rightX, settings.rightSensitivityX);
	state.RightStickY = toAxis(sticks.rightY, settings.rightSensitivityY);
}

void
WiiPadCaptureMouse(CMouseControllerState &state)
{
	state.Clear();
	state.x = 0.0f;
	state.y = 0.0f;

	// Same exclusivity as WiiPadCapture: any GameCube pad silences IR so a
	// live remote cannot steer the camera while driving with GC.
	if(s_connectedGameCubePads != 0){
		stopPointerHold();
		releaseCrosshair();
		return;
	}

	WPADData *data = WPAD_Data(WPAD_CHAN_0);
	if(data == nullptr){
		stopPointerHold();
		releaseCrosshair();
		return;
	}

	const bool tracked = data->ir.valid != 0;

	const bool aimWithPointer = !FrontEndMenuManager.m_bMenuActive && pointerAimWanted();
	if(!aimWithPointer)
		releaseCrosshair();

	if(FrontEndMenuManager.m_bMenuActive){
		// A cursor, absolutely: aiming at an option has to put the cursor on that
		// option.  The frontend clamps and re-reads these itself.  Nothing is held
		// here -- a cursor that carried on drifting after the pointer was lost
		// would walk off the option the player had just settled on.
		if(tracked){
			FrontEndMenuManager.m_nMouseTempPosX = (int32)data->ir.x;
			FrontEndMenuManager.m_nMouseTempPosY = (int32)data->ir.y;
		}

		// The click, and only in here.  In game these same two buttons are already
		// Circle and Cross on the pad state, and CControllerConfigManager::
		// AffectPadFromMouse would bind them a second time to whatever the mouse
		// is configured for -- one press firing two actions.  Read whether or not
		// the pointer is tracked, so a click still lands on wherever the cursor
		// was last left.
		state.LMB = (data->btns_h & WPAD_BUTTON_B) != 0;
		state.RMB = (data->btns_h & WPAD_BUTTON_A) != 0;
		stopPointerHold();
		return;
	}

	if(tracked){
		float rateX, rateY;
		bool turning;
		if(aimWithPointer){
			float crosshairX, crosshairY;
			turning = irAimRate(*data, crosshairX, crosshairY, rateX, rateY);
			steerCrosshair(crosshairX, crosshairY);
		}else
			turning = irPointerRate(*data, rateX, rateY);
		if(turning){
			s_heldRateX = rateX;
			s_heldRateY = rateY;
			s_heldSeconds = 0.0f;
		}else{
			// Aimed near the middle on purpose, which is a request to STOP rather
			// than a tracking dropout.  Nothing to hold.
			stopPointerHold();
		}
	}else{
		// Aimed past the edge of the sensor's field while still turning.  Keep
		// going the same way until the hold runs out.  The crosshair stays where
		// the pointer last left it for as long as that lasts, and then goes back
		// to rest rather than sit in a corner for a remote that was put down.
		s_heldSeconds += s_pointerDt;
		if(s_heldSeconds >= kPointerHoldSeconds){
			stopPointerHold();
			if(aimWithPointer)
				steerCrosshair(kAimDefaultX, kAimDefaultY);
		}
	}

	if(s_heldRateX == 0.0f && s_heldRateY == 0.0f)
		return;

	// Cam.cpp only reaches for the mouse when one of these is non-zero and
	// otherwise falls back to CPad::LookAroundLeftRight, so the dead zone above
	// is also what hands the camera back to the sticks.  The inversion flags are
	// the frontend's, applied the same way the DirectInput path applies them.
	const float deltaX = s_heldRateX*s_pointerDt;
	const float deltaY = s_heldRateY*s_pointerDt;
	state.x = MousePointerStateHelper.bInvertHorizontally ? -deltaX : deltaX;
	state.y = MousePointerStateHelper.bInvertVertically ? -deltaY : deltaY;
}

void
WiiPadUpdateRumble(void)
{
	CPad *pad = CPad::GetPad(0);

	// Spending the duration down is this backend's job, the same way it is the
	// XInput path's in Pad.cpp and glfw's in glfw.cpp.  Without it StartShake
	// only ever raises ShakeDur and the motors would never stop.
	if(pad->ShakeDur < CTimer::GetTimeStepInMilliseconds())
		pad->ShakeDur = 0;
	else
		pad->ShakeDur -= CTimer::GetTimeStepInMilliseconds();
	if(pad->ShakeDur == 0)
		pad->ShakeFreq = 0;

	// Both motors are on/off, with no speed to set, but the game asks for a strength
	// (ShakeFreq, 0-255, which the XInput path scales straight onto the motor).  The
	// way to get one out of an on/off motor is to pulse it: a running total of the
	// duty cycle asked for decides, frame by frame, whether the motor is on, and the
	// motor's own spin-up smooths the pulses into something weaker than full.  Never
	// below kRumbleMinDuty, or a light event would be too faint to feel at all.
	constexpr float kRumbleMinDuty = 0.35f;
	static float s_rumblePhase;
	int running = 0;
	if(pad->ShakeFreq != 0){
		s_rumblePhase += kRumbleMinDuty + (1.0f - kRumbleMinDuty)*((float)pad->ShakeFreq/255.0f);
		if(s_rumblePhase >= 1.0f){
			s_rumblePhase -= 1.0f;
			running = 1;
		}
	}else
		s_rumblePhase = 0.0f;
	WPAD_Rumble(WPAD_CHAN_0, running);
	PAD_ControlMotor(PAD_CHAN0, running ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
}

WiiConnectedPad
WiiPadQueryPrimary(void)
{
	// Prefer whatever is already plugged into a GameCube port -- that is the
	// device that also silences Wiimote input in WiiPadCapture.
	if(s_connectedGameCubePads != 0)
		return WII_PAD_GAMECUBE;

	for(int channel = 0; channel < WPAD_MAX_WIIMOTES; channel++){
		WPADData *data = WPAD_Data(channel);
		if(data == nullptr || data->err != WPAD_ERR_NONE)
			continue;
		const u32 expansion = probeExpansion(channel, *data);
		if(expansion == WPAD_EXP_CLASSIC)
			return WII_PAD_CLASSIC;
		if(expansion == WPAD_EXP_NUNCHUK)
			return WII_PAD_WIIMOTE_NUNCHUK;
		return WII_PAD_WIIMOTE;
	}
	return WII_PAD_NONE;
}

const char *
WiiPadPrimaryName(WiiConnectedPad pad)
{
	switch(pad){
	case WII_PAD_GAMECUBE: return "GameCube Controller";
	case WII_PAD_CLASSIC: return "Classic Controller / Classic Pro";
	case WII_PAD_WIIMOTE_NUNCHUK: return "Wii Remote + Nunchuk";
	case WII_PAD_WIIMOTE: return "Wii Remote";
	default: return "No controller detected";
	}
}

