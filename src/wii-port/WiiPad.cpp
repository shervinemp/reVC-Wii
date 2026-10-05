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
#include "ModelIndices.h"
#include "Pad.h"
#include "PlayerPed.h"
#include "Timer.h"
#include "Vehicle.h"
#include "WiiPad.h"
#include "WiiPointerAim.h"
#include "WiiSpeaker.h"
#include "WiiTrace.h"
#include "World.h"
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
//   curve       how much of the response is linear rather than quadratic.
//
// Halved, 0.22 -> 0.11, and this is the biggest single lever on how the aim feels.
//
// The dead zone and the aim box are two dead zones in series: the box stops the
// camera turning while the player aims inside the screen, and this stops it for a
// further stretch past the box edge.  They compound, and with the medium box at
// half-extent 0.27 they came to 0.27 + 0.22 = 0.49 of the available travel
// producing no camera movement at all, with only 0.23 of ramp left to carry the
// entire speed range.  That 0.23 is about 55 pixels at 640x480, so the whole
// control lived in a band 9% of the screen high, and reaching full speed took half
// that.  Measured like that, "coarse" is not a vague complaint: it is a control
// whose entire speed range is 55 pixels wide.
//
// Halving it roughly doubles the ramp without touching the aim area or the top
// speed, and without moving the saturation point, so both ends of the range are
// unchanged.  The cost is that pointer tremor within about 13 pixels of the box
// edge now nudges the camera.  That is the right way round for this complaint --
// coarse aim is something you live with, twitchy aim is something you fight -- and
// it is less bad than it sounds, because inside the box the reticle still tracks
// the pointer exactly, so the camera drift is cosmetic rather than disorienting.
constexpr float kPointerDeadzone = 0.11f;
// Where the linear ramp reaches full turn rate.
//
// Was 0.75, which is further than either axis can actually travel: at 640x448 the
// medium aim box leaves 0.77 half-heights horizontally past its edge but only
// 0.54 vertically, so horizontal saturated and vertical topped out around 63%.
// Two axes on two different parts of the same curve cannot be balanced by one
// pitch factor -- it is not a scale, it is a different shape. 0.45 is inside both
// ranges, so BOTH axes reach full rate and kPointerPitchScale becomes a plain
// balance between them.
constexpr float kPointerSaturation = 0.45f;
// When the pointer is off the sensor entirely the turn rate is held at this
// fraction of the maximum instead of running away, so losing the remote never
// spins the camera.
constexpr float kPointerOffScreenMaxFrac = 0.5f;

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
constexpr float kPointerRatePerSec = 420.0f;

// How much a deliberate sweep raises the gain, and how fast the hand has to be
// travelling before it counts as deliberate.  Applied ONLY when the hand is
// moving the same way the camera is already turning, so a counter-move gets no
// boost -- without that, pulling against a turn would speed the camera up and
// you would be fighting it.  A still hand scores zero, which is the important
// property: aiming is a still hand with small adjustments, so precise aim is
// bit-for-bit unchanged and only a deliberate sweep gets more camera.
constexpr float kSwingGainBoost = 0.45f;		// up to +45% on a fast same-way sweep
constexpr float kSwingGainFullAt = 1.2f;		// hand speed (g/s) that earns all of it
// The same delta pitches further than it yaws: Cam.cpp scales the vertical one
// by 4.0*m_fMouseAccelVertical against 2.5*m_fMouseAccelHorzntl for the
// horizontal, and m_fMouseAccelVertical is m_fMouseAccelHorzntl + 0.0005, so at
// the default settings pitch comes out about twice as fast as yaw.  This takes
// most of that back out but leaves the pitch still a little under the yaw, the
// way the stick path's own 0.6 factor in Cam.cpp does.  It was raised to 0.5 to
// chase "up/down is sluggish", which was the wrong diagnosis: the sluggishness
// was the aim box sitting off-centre (see kAimBoxes), and once that is fixed 0.32
// is the value that matches the stick, so it is back there.
constexpr float kPointerPitchScale = 0.26f;

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

// --- which way the hand is still going, once the pointer is off the bar -------
// The hold above replays the last turn rate blindly, which is right for finishing
// a turn that ran out of sensor and wrong the moment the player turns back: the
// camera carries on the way it was already going, and a remote set down on the
// sofa is indistinguishable from a hand that is still sweeping.  The Wiimote's
// own accelerometer can tell those apart.
//
// A raw acceleration reading cannot do it on its own.  Sweeping right reads
// right while the hand speeds up and left while it slows down again, so only the
// integral survives the whole gesture.  The deviation from a slowly followed
// gravity baseline -- the same trick the flick detector uses for its baseline --
// is integrated into a leaky velocity estimate, and the leak is what makes a hand
// that has stopped read as stopped rather than as wherever the last sweep left it.
//
// This is only ever allowed to CONTINUE a turn that is already happening, never to
// start one, so a remote merely lying down -- which is nothing but noise here --
// cannot turn the camera at all.
constexpr float kSwingGravityFollow = 0.06f;	// per scan: how fast gravity is followed
constexpr float kSwingLeakPerSec = 1.6f;		// how fast "not moving" comes to be believed
constexpr float kSwingThreshold = 0.12f;		// velocity, in g per second, that counts as a sweep

// --- Nunchuk flick-down jump -------------------------------------------------
// WiiPadScan measures the gesture and raises s_flickJumpPulse for exactly one
// frame; captureWiimote folds that into Square, the field JumpJustDown reads.
// A flick is a jolt in the Nunchuk's acceleration measured against the gravity
// it is already carrying, as a fraction of g.  Measuring the vector magnitude
// rather than one axis's per-frame delta is what makes it work in practice: a
// delta between two consecutive scans misses the peak whenever a scan lands
// between the flick and its return, which is why the old version felt janky and
// easy to miss.  A fraction of the live baseline is independent of the pad's raw
// accel scale and of how the remote happens to be held, and the settle window
// turns the rebound into "not armed yet" instead of a second, phantom jump.
static bool s_flickJumpPulse = false;
static const float kFlickGravityFollow = 0.04f;  // slow baseline follow, per scan
static const float kFlickFraction = 0.65f;       // a jolt must exceed ~65% of g: a decisive flick only
static const float kFlickRearmFraction = 0.35f;  // "settled" below ~35% of g
static const float kFlickSettleSec = 0.09f;      // stay deaf this long after a flick
static const float kFlickDownAlign = 0.55f;      // must still point into gravity: down-flick only

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
constexpr float kAimDefaultX = 0.5f;	// CCamera::Init's resting crosshair
constexpr float kAimDefaultY = 0.5f;
// The field order matches the order the literals below are written in.  It used to
// be {left, right, top, bottom}, which is an easy trap: a box written as
// "top, bottom, left, right" and read as "left, right, top, bottom" still compiles,
// still looks centred, and silently swaps which axis gets the off-centre pair.
struct AimBox
{
	float top, bottom, left, right;
};
constexpr AimBox kAimBoxes[] = {
	// Both pairs are centred on 0.5, and that is the whole point of the box.  The
	// turn radius is the gap between a box edge and the screen edge, so a box that
	// sits off-centre gives one direction more room to reach full deflection than
	// the other: the roomier side saturates the rate curve and turns fast, the
	// other runs out of screen before it ever does and crawls.  Both axes have to
	// be centred or the same bug reappears on whichever one is not.
	//
	// THIS is the dead zone.  The box is how far the crosshair may roam before the
	// camera is asked to turn at all, so widening it pushes the turn onset towards
	// the screen edge and lets a swing of the wrist move the aim without moving the
	// view -- which is what "the dead zone feels too small" means.  kPointerDeadzone
	// below is a different, much smaller thing: how far past the box edge the
	// pointer has to be before the turn starts at all.  Every box is square, so
	// neither axis has an advantage over the other at any setting.
	{ 0.31f, 0.69f, 0.31f, 0.69f },	// small  (half-extent 0.19)
	{ 0.23f, 0.77f, 0.23f, 0.77f },	// medium (half-extent 0.27, the default)
	{ 0.14f, 0.86f, 0.14f, 0.86f },	// large  (half-extent 0.36)
};
// The smoothing below is the only response shaping the crosshair gets; the turn
// past the box edge is linear all the way to kPointerSaturation, shared with the
// plain rate camera so the two feel like the same instrument.
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

// The turn rate the camera is actually using, eased towards whatever the response
// curve asks for.  Shared by both pointer paths -- the aiming one and the plain
// rate camera -- because they are never active at the same time and because the
// two are meant to feel like the same instrument.  See applyTurnSpinUp.
float s_turnRate;

// --- carrying the reticle on past the edge of the sensor bar -----------------
// The remote stops being tracked the instant it leaves the bar's field, and the
// last known screen position says which edge it went through.  Replaying the last
// turn rate alone loses that: the camera keeps moving while the reticle sits
// still, so aiming dies silently and the player is told nothing.
//
// So the reticle is carried on in the direction it was already travelling and
// clamped to the edge it left through, which is both the physically honest guess
// and the legible one -- the dot visibly parks against the edge of the screen
// instead of vanishing.  Position is the raw target rather than the smoothed
// crosshair, so the velocity is the player's own and not the filter's.
static float s_lastTargetX;
static float s_lastTargetY;
// The reticle's own speed while it was still tracked, per second.  This is what
// the carry-on is seeded from, so it continues at exactly the rate the player was
// already seeing rather than at a rate invented from the hand.
static float s_ptrVelX;
static float s_ptrVelY;
// The carry-on itself: a velocity seeded from the above and steered by the hand,
// and the virtual position it has produced, clamped to the screen.
static float s_lostVelX;
static float s_lostVelY;
static float s_lostX;
static float s_lostY;
static bool s_lostActive;

// How fast that carried-on velocity bleeds away.  Per second, so a sweep coasts
// to a stop over a few frames instead of running at the edge forever.
static const float kLostCoastDecay = 0.35f;
// How fast a hand's motion carries the reticle while the bar is invisible, in
// screen fractions per second per g/s of hand speed.  Sized so a deliberate sweep
// walks the dot to the edge of the screen in roughly the time the sweep took to
// leave it, rather than teleporting there or crawling.
static const float kLostHandGain = 0.55f;
// The accelerometer's speed reading, in g per second, shared between the
// off-screen test and the aim gain so both read one measurement rather than two
// integrations of the same accelerometer.
static float s_handSpeed;
// The hand's direction, unit length, straight from the accelerometer.  Kept
// separate from s_lostVelX/Y because that one is the carry-on velocity and goes
// stale; this is refreshed every frame from the sensor and is what the aim gain
// judges intent by.
static float s_handDirX;
static float s_handDirY;

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

	// Face buttons: A=Cross, B=Circle, X=Square, Y=Triangle.  In v3, A is the gas
	// (Cross, the "commit" button) and B the fire (Circle, the "shoot" button).
	// Same field layout the Wiimote gets, so the Mode 0 rebinds read the same
	// whichever pad is in hand.
	setButton(state.Circle, buttons & PAD_BUTTON_B);   // B: fire (foot) / fire+drive-by (car)
	setButton(state.Cross, buttons & PAD_BUTTON_A);    // A: enter+sprint (foot) / gas (car)
	setButton(state.Square, buttons & PAD_BUTTON_X);   // X: jump
	setButton(state.Triangle, buttons & PAD_BUTTON_Y); // Y: exit vehicle
	setButton(state.Start, buttons & PAD_BUTTON_START);

	// L is the brake (RightShoulder1), Z the horn (LeftShoulder1), R the radio.
	setButton(state.RightShoulder1, buttons & PAD_TRIGGER_L);
	setButton(state.LeftShoulder1, buttons & PAD_TRIGGER_Z);
	setButton(state.LeftShock, buttons & PAD_TRIGGER_R);

	const bool dpadLeft = (buttons & PAD_BUTTON_LEFT) != 0;
	const bool dpadRight = (buttons & PAD_BUTTON_RIGHT) != 0;
	const bool dpadDown = (buttons & PAD_BUTTON_DOWN) != 0;
	const bool dpadUp = (buttons & PAD_BUTTON_UP) != 0;
	if(!FrontEndMenuManager.m_bMenuActive){
		// vehicle cluster
		setButton(state.LeftShoulder2, dpadLeft || dpadUp);
		setButton(state.RightShoulder2, dpadRight || dpadUp);
		setButton(state.DPadDown, dpadDown);
		setButton(state.RightShock, dpadUp);
	}else{
		setButton(state.DPadUp, dpadUp);
		setButton(state.DPadDown, dpadDown);
		setButton(state.DPadLeft, dpadLeft);
		setButton(state.DPadRight, dpadRight);
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

	// Same v3 field layout as the Wiimote and the GameCube pad, so the Mode 0
	// rebinds read the same whichever pad is in hand.  Physical A is the gas
	// (Cross, the "commit" button) and B the fire (Circle, the "shoot" button),
	// L the brake (RightShoulder1), R the horn (LeftShoulder1), and the D-pad the
	// vehicle cluster when it has two sticks of its own to walk with.
	setButton(state.Cross, buttons & WPAD_CLASSIC_BUTTON_A);    // A: enter+sprint (foot) / gas (car)
	setButton(state.Circle, buttons & WPAD_CLASSIC_BUTTON_B);   // B: fire (foot) / fire+drive-by (car)
	setButton(state.Square, buttons & WPAD_CLASSIC_BUTTON_Y);   // Y: jump
	setButton(state.Triangle, buttons & WPAD_CLASSIC_BUTTON_X); // X: exit vehicle
	setButton(state.RightShoulder1, buttons & WPAD_CLASSIC_BUTTON_FULL_L); // L: brake
	setButton(state.LeftShoulder1, buttons & WPAD_CLASSIC_BUTTON_FULL_R);  // R: horn
	setButton(state.Start, buttons & WPAD_CLASSIC_BUTTON_PLUS);
	setButton(state.Select, buttons & WPAD_CLASSIC_BUTTON_MINUS);

	if(!FrontEndMenuManager.m_bMenuActive){
		// Two sticks of their own walk and steer, so the D-pad is free to be the
		// vehicle cluster whenever it is not driving a menu.
		const bool dpadLeft = (buttons & WPAD_CLASSIC_BUTTON_LEFT) != 0;
		const bool dpadRight = (buttons & WPAD_CLASSIC_BUTTON_RIGHT) != 0;
		const bool dpadDown = (buttons & WPAD_CLASSIC_BUTTON_DOWN) != 0;
		const bool dpadUp = (buttons & WPAD_CLASSIC_BUTTON_UP) != 0;
		setButton(state.LeftShoulder2, dpadLeft || dpadUp);
		setButton(state.RightShoulder2, dpadRight || dpadUp);
		setButton(state.DPadDown, dpadDown);
		setButton(state.RightShock, dpadUp);
	}else{
		setButton(state.DPadUp, buttons & WPAD_CLASSIC_BUTTON_UP);
		setButton(state.DPadDown, buttons & WPAD_CLASSIC_BUTTON_DOWN);
		setButton(state.DPadLeft, buttons & WPAD_CLASSIC_BUTTON_LEFT);
		setButton(state.DPadRight, buttons & WPAD_CLASSIC_BUTTON_RIGHT);
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
	const bool dpadUp = (buttons & WPAD_BUTTON_UP) != 0;

	// v3 layout, Wiimote + Nunchuk:
	//   A  enter vehicle / sprint (foot)   accelerate (car)      <- the "commit" button
	//   B  fire (foot)                    fire, drive-by, car gun (car)  <- "shoot"
	//   Z  -- (the pointer aims)         brake and reverse (car)
	//   1  --                             radio (car)
	//   2  --                             exit vehicle (car)
	//   C  --                             horn (car)
	//   D-pad   cycle weapon / look behind (foot)
	//           handbrake / look L-R / look behind (car)
	//   +  pause (always)
	//   -  camera mode (foot)
	//   flick  jump; HOME is the system menu, not the game
	// The Nunchuk is required: the stick does all movement, so without one there is
	// nothing to walk or steer with.  Rather than pretend a bare Wiimote works, the
	// D-pad is always the cluster in-game and the boot screen blocks until a
	// Nunchuk is plugged in.  In the menus the D-pad goes back to being a D-pad.
	const bool dpadIsCluster = !FrontEndMenuManager.m_bMenuActive;

	setButton(state.Circle, buttons & WPAD_BUTTON_B);   // B: fire (foot) / fire+drive-by (car)
	setButton(state.Cross, buttons & WPAD_BUTTON_A);    // A: enter+sprint (foot) / gas (car)
	setButton(state.Triangle, buttons & WPAD_BUTTON_2); // 2: exit vehicle
	// Jump has two triggers: the Nunchuk flick (measured in WiiPadScan) or the 1
	// button on foot.  The flick alone proved janky and easy to miss in testing,
	// so 1 is the reliable way in -- on foot 1 is otherwise idle, and in a car 1 is
	// the radio and deliberately does not jump.  Both feed the one Square pulse
	// JumpJustDown reads, so a jump is a jump whichever you use.
	const bool jumpByOne = (buttons & WPAD_BUTTON_1) && !inCar;
	setButton(state.Square, s_flickJumpPulse || jumpByOne);

	// 1 is the radio station in a car, and nothing on foot: jump is the Nunchuk
	// flick (WiiSpeakerService/WiiPadScan), and sniper scope entry already comes
	// from the aim button (Z, which feeds RightShoulder1 -- PlayerPed.cpp:1281
	// reads TargetJustDown), so a second way into the scope would only duplicate
	// a state Z already owns.
	setButton(state.LeftShock, buttons & WPAD_BUTTON_1);   // 1: radio (car)
	// - is the camera mode on foot (the engine only reads it for a pedestrian
	// camera, so it is simply dead in a car).  Pause is + (the Wii menu button)
	// and is unconditional; HOME is left alone so the console's own HOME opens
	// the system menu instead of fighting an in-game handler.
	setButton(state.Select, buttons & WPAD_BUTTON_MINUS);
	setButton(state.Start, buttons & WPAD_BUTTON_PLUS);

	if(dpadIsCluster){
		// Left/right: weapon cycle on foot (LeftShoulder2 / RightShoulder2), and
		// the same two fields are what the engine reads as look left / right in a
		// car. Up is look behind, which in a car is those same two fields held
		// together, so it sets both. Down is the handbrake on its own field.
		setButton(state.LeftShoulder2, dpadLeft || dpadUp);
		setButton(state.RightShoulder2, dpadRight || dpadUp);
		setButton(state.DPadDown, dpadDown);
		// Look behind on foot reads RightShock, which nothing else uses now.
		setButton(state.RightShock, dpadUp);
	}else{
		// Menus only: the D-pad navigates as a D-pad.
		setButton(state.DPadUp, dpadUp);
		setButton(state.DPadDown, dpadDown);
		setButton(state.DPadLeft, dpadLeft);
		setButton(state.DPadRight, dpadRight);
	}

	if(!hasNunchuk)
		return;

	// libogc reports the expansion's buttons twice: merged into the Wiimote mask
	// (WPAD_NUNCHUK_BUTTON_* are the low bits shifted up by 16) and in the
	// expansion struct's own byte.  Either is normally enough; taking both costs
	// one OR and removes a whole class of "works on my Wiimote" difference.
	const nunchuk_t &nunchuk = data.exp.nunchuk;
	setButton(state.RightShoulder1, (buttons & WPAD_NUNCHUK_BUTTON_Z) ||
		(nunchuk.btns_held & NUNCHUK_BUTTON_Z));   // Z: brake and reverse
	setButton(state.LeftShoulder1, (buttons & WPAD_NUNCHUK_BUTTON_C) ||
		(nunchuk.btns_held & NUNCHUK_BUTTON_C));    // C: horn

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
// Shared turn-rate response.  `over` is the vector from the aim point (pointer
// centre or crosshair) to the pointer, in the normalised half-height units the
// callers use.  The response is QUADRATIC in how far outside the dead zone the
// pointer sits: equal movements still turn the camera by more the further out you
// are, but the curve is much finer near the box edge, which is where precision
// lives.  Once the pointer is off the sensor the rate is capped at
// kPointerOffScreenMaxFrac of the maximum rather than running away, so a lost
// remote cannot spin the view.
// Returns the scalar turn rate (0 when inside the dead zone).
float
pointerTurnRate(float magnitude, bool offScreen)
{
	if(magnitude <= kPointerDeadzone)
		return 0.0f;
	float t = (magnitude - kPointerDeadzone)/(kPointerSaturation - kPointerDeadzone);
	if(t > 1.0f)
		t = 1.0f;
	if(offScreen && t > kPointerOffScreenMaxFrac)
		t = kPointerOffScreenMaxFrac;
	// Squared, not linear.  The point of widening the ramp above was to buy back
	// resolution, and a linear ramp spends most of that resolution on the fast end
	// where nobody aims -- holding a sweep needs range, placing a shot does not.
	// t*t puts the fine control where it is useful and still reaches exactly the
	// same full rate at exactly the same pointer position, so nothing about the
	// ends of the range moves.  At the old deadzone the first live pixel gave a
	// third of full speed; it now gives a twentieth.
	return kPointerRatePerSec*t*t;
}

// How long the turn rate takes to catch up with what the ramp is asking for.
// Time based, not per frame, so it behaves the same if the frame rate moves.
constexpr float kTurnSpinUpTau = 0.06f;

// The rate the camera is actually turning at, as opposed to the rate the response
// curve is asking for.  Eased towards the target so that arriving at a speed is
// a movement rather than a step.
//
// The step it replaces was a real part of the coarseness, and not a small one: the
// camera used to go from stationary to the full rate the ramp allowed inside a
// single frame, so clearing the dead zone by one pixel was the difference between
// a still camera and a moving one.  With the whole speed range living in about 55
// pixels of travel, most of the time was spent slamming between two speeds.
//
// Eased on the way up and NOT on the way down, which is deliberate.  A spin-down
// would keep the camera drifting after the player had stopped asking for it, and
// a camera that will not stop where you told it to is a worse complaint than one
// that starts a touch softly.  Coming to a halt is instant; getting going is not.
float
applyTurnSpinUp(float targetRate)
{
	if(targetRate <= 0.0f){
		s_turnRate = 0.0f;
		return 0.0f;
	}
	if(targetRate <= s_turnRate){
		// Falling, or unchanged: track it directly so the camera can stop.
		s_turnRate = targetRate;
		return s_turnRate;
	}
	const float follow = 1.0f - std::exp(-s_pointerDt/kTurnSpinUpTau);
	s_turnRate += (targetRate - s_turnRate)*follow;
	return s_turnRate;
}

// Whether the pointer has left the sensor bar (its position is outside the
// screen).  Such a pointer is clamped to the edge for aiming but still steers,
// at a capped rate.
inline bool
pointerOffScreen(const WPADData &data)
{
	return data.ir.x < 0 || data.ir.y < 0 ||
		data.ir.x > RsGlobal.maximumWidth || data.ir.y > RsGlobal.maximumHeight;
}

int16
irPointerRate(const WPADData &data, float &outX, float &outY)
{
	const float width = (float)RsGlobal.maximumWidth;
	const float height = (float)RsGlobal.maximumHeight;
	if(width <= 0.0f || height <= 0.0f)
		return 0;

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
	const float rate = applyTurnSpinUp(pointerTurnRate(magnitude, pointerOffScreen(data)));
	if(rate <= 0.0f)
		return 0;

	// The unit direction comes from the raw vector, so only the SPEED goes
	// through the response.  Curving each axis on its own would bend diagonals
	// towards the nearer axis, the same directional error the normalisation
	// above avoids.
	outX = (unitX/magnitude)*rate;
	outY = (unitY/magnitude)*rate*kPointerPitchScale;
	return 1;
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

// The accelerometer's slow view of "which way down is", and the leaky velocity
// built on top of it.  File scope rather than a function static so it is one piece
// of state rather than a static buried in the middle of the capture path.
struct SwingTracker
{
	float gx, gy, gz;	// slowly followed gravity vector
	float vx, vy;		// velocity estimate, in g per second
	bool  seeded;
};
static SwingTracker s_swing = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false };

// Feeds one scan of the Wiimote's accelerometer through the tracker above and
// reports which way the hand is still travelling: +x right, +y DOWN, matching the
// screen axis the aim box uses, so the two can be dotted together directly.
// Returns false when the hand is not travelling far enough to read as a
// deliberate sweep rather than as tremor.
//
// Called every frame the pointer is live, tracked or not.  Only the untracked
// case acts on the answer, but a tracker that stops running while the bar is in
// sight has nothing useful left to say the moment it loses it.
bool
pointerSwing(const WPADData &data, float &outX, float &outY, float &speedOut)
{
	const float ax = (float)data.accel.x;
	const float ay = (float)data.accel.y;
	const float az = (float)data.accel.z;

	if(!s_swing.seeded){
		// First sample defines "up" for this grip.  Without it the first frames
		// after a boot would read as a violent sweep in an arbitrary direction.
		s_swing.gx = ax; s_swing.gy = ay; s_swing.gz = az;
		s_swing.seeded = true;
	}
	s_swing.gx += (ax - s_swing.gx)*kSwingGravityFollow;
	s_swing.gy += (ay - s_swing.gy)*kSwingGravityFollow;
	s_swing.gz += (az - s_swing.gz)*kSwingGravityFollow;

	const float g = std::sqrt(s_swing.gx*s_swing.gx + s_swing.gy*s_swing.gy + s_swing.gz*s_swing.gz);
	if(g <= 1.0f){
		// No usable reading (a sensor that has gone quiet reads flat, which is
		// nowhere near a g).  Say nothing rather than scale noise up.
		outX = 0.0f;
		outY = 0.0f;
		return false;
	}
	const float inv = 1.0f/g;

	// The deviation from the gravity the remote is already carrying, in fractions
	// of g, split into the part ALONG that gravity and the part ACROSS it.  A hand
	// sweeping left or right pushes across gravity and one moving up or down the
	// screen pushes along it, so the split is what tells the two apart -- and
	// unlike reading one fixed body axis it does not care how the remote happens
	// to be rolled or tilted in the hand.  At rest the accelerometer reads the
	// reaction to gravity, so the baseline direction is "up".
	const float dx = (ax - s_swing.gx)*inv;
	const float dy = (ay - s_swing.gy)*inv;
	const float dz = (az - s_swing.gz)*inv;
	const float ux = s_swing.gx*inv, uy = s_swing.gy*inv, uz = s_swing.gz*inv;
	const float along = dx*ux + dy*uy + dz*uz;
	const float acrossX = dx - along*ux;

	// Down the screen is the negative of "along": accelerating downwards reduces
	// the reading along the baseline, because the accelerometer reports specific
	// force and gravity is already in it.  Across gravity the body's X axis is the
	// screen's left/right for a remote held like a television remote, and the other
	// two across-axes barely move for that gesture, so X alone carries it.
	const float moveX = acrossX;
	const float moveY = -along;

	const float dt = s_pointerDt;
	const float keep = std::exp(-kSwingLeakPerSec*dt);
	s_swing.vx = s_swing.vx*keep + moveX*dt;
	s_swing.vy = s_swing.vy*keep + moveY*dt;

	const float speed = std::sqrt(s_swing.vx*s_swing.vx + s_swing.vy*s_swing.vy);
	speedOut = speed;
	if(speed < kSwingThreshold){
		outX = 0.0f;
		outY = 0.0f;
		return false;
	}
	// Unit direction with a magnitude of 1, so the sign of the dot product with a
	// held turn rate is all the caller needs.
	outX = s_swing.vx/speed;
	outY = s_swing.vy/speed;
	return true;
}

// Forgets the gravity baseline, so a remote that has been unplugged and plugged
// back in does not spend its first frames integrating the discontinuity as a
// sweep.  Nothing is held across a disconnect anyway, so this only has to be
// right rather than fast.
void
resetPointerSwing(void)
{
	s_swing.gx = s_swing.gy = s_swing.gz = 0.0f;
	s_swing.vx = s_swing.vy = 0.0f;
	s_swing.seeded = false;
}

// Whether the pointer should be moving the crosshair this frame: a gun out whose
// crosshair the HUD shows (the same test as Hud.cpp), in the Standard method, and
// the option on.  Everything else keeps the fixed crosshair and the rate camera.
bool
pointerAimWanted(void)
{
	if(!WiiPointerAimEnabled || !CCamera::m_bUseMouse3rdPerson)
		return false;
	CPlayerPed *player = FindPlayerPed();
	// Getting in or out of a car is excluded because the camera is mid-hand-over
	// there and the crosshair is about to be claimed by the car.  Driving itself
	// is NOT excluded: that exclusion was what made the pointer dead behind the
	// wheel, and it was redundant anyway, because Using3rdPersonMouseCam() was
	// false for every frame of a drive (the camera is MODE_BEHINDCAR, not
	// MODE_FOLLOWPED) and has now been taught to answer true there.  Keeping the
	// vehicle case would have meant the function returned false for both reasons
	// at once, and fixing only one of them would have changed nothing.
	if(player == nullptr ||
	   player->m_nPedState == PED_ENTER_CAR || player->m_nPedState == PED_CARJACK)
		return false;
	if(!TheCamera.Cams[TheCamera.ActiveCam].Using3rdPersonMouseCam())
		return false;
	// No weapon required.  The crosshair follows the pointer whether or not a gun
	// is out, so the pointer position is always visible (the HUD draws a small dot
	// when unarmed) and the shot ray keeps tracing through the same point.  The
	// weapon used to gate this, which is why there was nothing to aim with until
	// you drew a gun.
	return true;
}

// Gives the crosshair back to the game.
void
releaseCrosshair(void)
{
	s_aimActive = false;
	// So drawing the next weapon starts from a standstill and eases up, rather than
	// inheriting whatever speed the last one was released at.
	s_turnRate = 0.0f;
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

// The pointer's two jobs while it owns the crosshair.  The crosshair tracks the
// pointer across the WHOLE screen (so you can aim anywhere, not just a small
// centre box), and the camera turns only once the pointer is pushed past the
// screen edge.  The WII_BOX option now controls how far in from that edge the
// turn begins: a smaller box turns sooner, a larger one keeps the camera still
// nearer the edge.  The turn is linear in how far outside the edge the pointer
// is and is capped at half the maximum once the pointer is off the sensor.
// Returns false when the pointer is inside the turn ring (pure aiming, camera
// still), which is what leaves the camera steady while the player aims.
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
	// The crosshair gets the full screen so aiming is never boxed in.
	const float clampX = pointerX < 0.0f ? 0.0f : (pointerX > 1.0f ? 1.0f : pointerX);
	const float clampY = pointerY < 0.0f ? 0.0f : (pointerY > 1.0f ? 1.0f : pointerY);
	outCrosshairX = clampX;
	outCrosshairY = clampY;

	// The camera turns only past a margin in from the screen edge (the box, now
	// used as an edge ring).  Inside it, pointer == clamped position, no turn.
	const float turnL = box.left, turnR = box.right, turnT = box.top, turnB = box.bottom;
	const float heldX = pointerX < turnL ? turnL : (pointerX > turnR ? turnR : pointerX);
	const float heldY = pointerY < turnT ? turnT : (pointerY > turnB ? turnB : pointerY);
	const float half = height*0.5f;
	const float overX = (pointerX - heldX)*width/half;
	const float overY = (pointerY - heldY)*height/half;
	const float magnitude = std::sqrt(overX*overX + overY*overY);

	// Not returned early on zero.  The spin-up at the end has to see the zero in
	// order to snap a falling rate to a stop, so this only computes the target.
	float rate = pointerTurnRate(magnitude, pointerOffScreen(data));
	// Gain from intent: a deliberate sweep that AGREES with the turn the camera is
	// already making gets more camera, up to +45%.  The agreement test is the whole
	// design -- without it, hauling the hand back to reverse a turn would speed the
	// camera up and you would be fighting it, so a counter-move must score zero.
	// A still hand scores zero too, which is what keeps precise aim untouched:
	// aiming is a still hand with small adjustments, so the boost only ever appears
	// on a sweep, which is exactly when more camera is wanted.
	if(s_handSpeed > 0.0f && magnitude > 0.0f){
		const float turnX = overX/magnitude;
		const float turnY = overY/magnitude;
		// The hand's direction, in the same screen axes the turn uses, so the dot
		// product is the cosine between them.  s_handDirX/Y is unit length or zero:
		// pointerSwing zeroes it below its noise threshold rather than reporting a
		// stale direction, which is what keeps a hand at rest from boosting on the
		// strength of a direction it is no longer travelling in.
		const float alignment = s_handDirX*turnX + s_handDirY*turnY;
		if(alignment > 0.0f){
			const float strength = Min(1.0f, s_handSpeed/kSwingGainFullAt);
			rate *= 1.0f + kSwingGainBoost*alignment*strength;
			// Ceilinged, because the boost multiplies a rate that is already at
			// the top of its ramp, and unclamped a fast agreeing sweep landed
			// well past anything the finger can follow.
			if(rate > kPointerRatePerSec)
				rate = kPointerRatePerSec;
		}
	}

	// Applied here rather than inside pointerTurnRate, so that the intent gain above
	// is part of what gets eased.  Easing the ramp and then boosting on top would
	// let the boost reintroduce the very step the spin-up exists to remove.
	const float applied = applyTurnSpinUp(rate);
	if(applied <= 0.0f)
		return false;

	outX = (overX/magnitude)*applied;
	outY = (overY/magnitude)*applied*kPointerPitchScale;
	return true;
}

} // namespace

int8_t WiiPointerAimEnabled = 1;
int8_t WiiPointerBox = 1;

// Outside the anonymous namespace: the boot gate in wii_game.cpp calls this.
bool
WiiPadNunchukConnected(void)
{
	const WPADData *data = WPAD_Data(WPAD_CHAN_0);
	if(data == nullptr || data->err != WPAD_ERR_NONE)
		return false;
	u32 expansion = WPAD_EXP_NONE;
	if(WPAD_Probe(WPAD_CHAN_0, &expansion) != WPAD_ERR_NONE)
		return false;
	if(expansion == WPAD_EXP_NONE)
		expansion = (u32)data->exp.type;
	return expansion == WPAD_EXP_NUNCHUK;
}

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

	// HOME returns to the Wii system menu (the native "home"), the way a console
	// title does.  A homebrew cannot just let the system take HOME -- that is why
	// it did nothing here -- so this asks SYS_ResetSystem to load the Wii Channels
	// menu directly.  It is worth warning first: returning resets the app and
	// throws away everything since the last save, so from a running game it asks
	// through the frontend's own "quit game?" screen and quits outright from the
	// title screens (nothing at risk).  SYS_RETURNTOMENU needs the IOS to allow it;
	// if it refuses, fall back to HandleExit so HOME always gets you out.
	if(WPAD_ButtonsDown(WPAD_CHAN_0) & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)){
		const bool onQuitScreen = FrontEndMenuManager.m_bMenuActive &&
			FrontEndMenuManager.m_nCurrScreen == MENUPAGE_EXIT;
		if(FrontEndMenuManager.m_bGameNotLoaded || FrontEndMenuManager.m_bQuitPromptRequested || onQuitScreen){
			WiiTraceReport("WII pad: HOME pressed, returning to the Wii menu\n");
			// Best effort: straight to the Wii Channels menu.
			SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
			// If that did not take us out, fall back to the loader.
			HandleExit();
		}else{
			WiiTraceReport("WII pad: HOME pressed, asking\n");
			FrontEndMenuManager.RequestQuitPrompt();
		}
	}

	// Nunchuk flick-down = jump (one of two triggers; 1 on foot is the other).
	// The gesture is read here, once per scan, and published as a one-frame Square
	// pulse that captureWiimote folds into the pad state (JumpJustDown reads
	// Square).  It fires on a jolt in the acceleration vector measured against the
	// gravity the Nunchuk is already carrying -- see the constants above.  Three
	// things keep it honest:
	//   - it is a transient jolt above a live gravity baseline, not a tilt angle,
	//     so holding the remote still never reads as a flick;
	//   - the threshold is a fraction of g, so it does not care how the pad's raw
	//     accel happens to be scaled or how the remote is held;
	//   - a settle window makes the rebound at the end of a flick re-arm the
	//     detector rather than fire a second, phantom jump.
	// It is on foot only and idle while the stick is deflected, because steering
	// and running shake the Nunchuk constantly.
	{
		static bool   s_jumpPulse = false;
		static float  s_gravity = 0.0f;   // slow |accel| at rest, about one g
		static float  s_gx = 0.0f, s_gy = 0.0f; // slow gravity vector: which way is down
		static bool   s_settling = false;  // deaf while a flick rings down
		static float  s_settleT = 0.0f;
		s_jumpPulse = false;

		const WPADData *wd = WPAD_Data(WPAD_CHAN_0);
		CPlayerPed *ped = FindPlayerPed();
		const bool nunchukReady = wd != nullptr && wd->err == WPAD_ERR_NONE &&
			wd->exp.type == WPAD_EXP_NUNCHUK;
		const bool onFoot = ped != nullptr && !ped->bInVehicle;
		// Menus run with the player ped still alive on foot, so without this the
		// flick would pulse Square behind an open menu.  Gameplay input is paused
		// in menus anyway, so the pulse would go nowhere -- this just keeps the
		// gesture honest.
		const bool inMenu = FrontEndMenuManager.m_bMenuActive;

		if(!nunchukReady || !onFoot || inMenu){
			s_settling = false;
			s_settleT = 0.0f;
		}else{
			const s16 ax = wd->exp.nunchuk.accel.x;
			const s16 ay = wd->exp.nunchuk.accel.y;
			const s16 az = wd->exp.nunchuk.accel.z;
			const float mag = std::sqrt((float)ax*ax + (float)ay*ay + (float)az*az);
			if(s_gravity <= 0.0f){
				s_gravity = mag;                    // first sample seeds the baseline
				s_gx = (float)ax; s_gy = (float)ay;
			}else{
				s_gravity += (mag - s_gravity)*kFlickGravityFollow;
				s_gx += ((float)ax - s_gx)*kFlickGravityFollow;
				s_gy += ((float)ay - s_gy)*kFlickGravityFollow;
			}
			// How far the current acceleration sits above the gravity baseline, as a
			// fraction of that baseline.  A deliberate flick spikes this; holding,
			// walking, or steering do not.
			const float dev = (mag - s_gravity)/(s_gravity > 1.0f ? s_gravity : 1.0f);
			// Which way is "down" right now, from the slowly-followed gravity
			// vector (not the raw sample, which is what the jolt perturbs).  At rest
			// the accelerometer already reads the gravity vector, so this alignment
			// is about 1 however the remote is held.  A down-flick pushes further
			// along gravity and keeps it near 1; an up-flick opposes gravity and
			// drops it, so requiring real alignment rejects up-flicks.
			const float glen = std::sqrt(s_gx*s_gx + s_gy*s_gy);
			const float align = glen > 1.0f ? ((float)ax*s_gx + (float)ay*s_gy)/glen : 1.0f;
			const bool wentDown = align > kFlickDownAlign;
			if(s_settling){
				// Wait for the jolt and its rebound to fall away before arming again,
				// so one flick is exactly one pulse.
				if(dev > kFlickRearmFraction)
					s_settleT = 0.0f;
				else
					s_settleT += s_pointerDt;
				if(s_settleT >= kFlickSettleSec)
					s_settling = false;
			}else if(dev > kFlickFraction && wentDown){
				s_jumpPulse = true;
				s_settling = true;
				s_settleT = 0.0f;
			}
		}
		s_flickJumpPulse = s_jumpPulse;
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
		resetPointerSwing();
		return;
	}

	WPADData *data = WPAD_Data(WPAD_CHAN_0);
	if(data == nullptr){
		stopPointerHold();
		releaseCrosshair();
		resetPointerSwing();
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
		// The tracker is not run in here, so its last velocity would still be
		// sitting there on the first frame back out.
		resetPointerSwing();
		return;
	}

	// Runs every frame, in view or not, so the velocity estimate is never stale at
	// the moment the bar is lost -- which is the only moment it is read.
	float swingX = 0.0f, swingY = 0.0f;
	bool sweeping = false;
	if(data->err == WPAD_ERR_NONE){
		sweeping = pointerSwing(*data, swingX, swingY, s_handSpeed);
		s_handDirX = swingX;
		s_handDirY = swingY;
	}else{
		// A remote that has gone quiet leaves the accelerometer flat; integrating
		// that as motion would be the one thing that could start a turn from
		// nothing, so the tracker is dropped instead.
		resetPointerSwing();
	}

	if(!tracked){
		// Carrying the reticle on past the edge of the bar, from the MOTION SENSOR
		// rather than from the pointer.  This is the difference that matters: the
		// pointer's last known velocity is frozen the instant tracking dies, so it
		// knows the direction the hand was going and nothing else.  The
		// accelerometer keeps reporting the whole time the bar is out of sight, so
		// the direction -- and how hard -- is still live.  That makes the carry-on
		// steerable: you can keep aiming off-screen by moving your hand, instead of
		// watching a dot that has stopped and a camera that has not.
		//
		// Clamped to the screen, so it parks against the edge it left through.  That
		// is the legible part -- the player can see they have run out of sensor --
		// and it is why the sensor is worth using rather than just coasting on a
		// remembered number.
		if(!s_lostActive){
			s_lostActive = true;
			s_lostX = s_lastTargetX;
			s_lostY = s_lastTargetY;
			// Seeded from the reticle's OWN last speed, not the hand's.  Deriving it
			// from hand speed alone made the dot travel at a different rate than the
			// motion it was standing in for, which is a visible discontinuity the
			// moment the bar comes back.  The hand only steers it from here.
			s_lostVelX = s_ptrVelX;
			s_lostVelY = s_ptrVelY;
			// Half speed while the bar is out.  That cap used to be unreachable:
			// it only ever applied to out-of-range coordinates, and losing the bar
			// outright is the common case by far.
			s_heldRateX *= kPointerOffScreenMaxFrac;
			s_heldRateY *= kPointerOffScreenMaxFrac;
		}
		if(sweeping){
			// The hand steers; it does not set the pace.  Direction from the sensor,
			// magnitude kept from the seed so the speed the reticle was already
			// travelling at is the speed it carries on at, and only bleeds away once
			// the hand stops.
			const float carried = std::sqrt(s_lostVelX*s_lostVelX + s_lostVelY*s_lostVelY);
			const float wanted = s_handSpeed*kLostHandGain;
			if(wanted > 0.0f){
				s_lostVelX = swingX*wanted;
				s_lostVelY = swingY*wanted;
			}else if(carried > 0.0f){
				s_lostVelX = s_lostVelX/carried*wanted;
				s_lostVelY = s_lostVelY/carried*wanted;
			}
		}else{
			// The hand has stopped, so coast on what it was doing and bleed away
			// rather than sliding along the edge indefinitely.
			s_lostVelX -= s_lostVelX*kLostCoastDecay*s_pointerDt;
			s_lostVelY -= s_lostVelY*kLostCoastDecay*s_pointerDt;
		}
		s_lostX += s_lostVelX*s_pointerDt;
		s_lostY += s_lostVelY*s_pointerDt;
		if(s_lostX < 0.0f) s_lostX = 0.0f; else if(s_lostX > 1.0f) s_lostX = 1.0f;
		if(s_lostY < 0.0f) s_lostY = 0.0f; else if(s_lostY > 1.0f) s_lostY = 1.0f;
		if(aimWithPointer)
			steerCrosshair(s_lostX, s_lostY);
	}

	if(tracked){
		// Cleared here, not inside the aimWithPointer branch and not only when the
		// crosshair steers.  WiiPadCaptureMouse returns early while a menu is open, so
		// a pause taken while off the bar left this latched -- and the half speed cap
		// below is applied once on entry, so it was silently skipped for the rest of
		// the session after one pause.
		s_lostActive = false;
		float rateX, rateY;
		bool turning;
		if(aimWithPointer){
			float crosshairX, crosshairY;
			turning = irAimRate(*data, crosshairX, crosshairY, rateX, rateY);
			// How fast the reticle was actually travelling, so that losing the bar
			// halfway through a sweep can carry on the same way instead of stopping
			// dead.  Divided by the frame time so it is per second and does not
			// depend on the frame rate, and taken before the smoothing filter so it
			// is the player's hand rather than the filter's lag.
			if(s_pointerDt > 0.0f){
				s_ptrVelX = (crosshairX - s_lastTargetX)/s_pointerDt;
				s_ptrVelY = (crosshairY - s_lastTargetY)/s_pointerDt;
			}
			s_lastTargetX = crosshairX;
			s_lastTargetY = crosshairY;
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
		// Aimed past the edge of the sensor's field while still turning.  The
		// accelerometer gets a vote here, because blind replay of the last rate is
		// right for one case and wrong for the other two.
		if(sweeping && (s_heldRateX != 0.0f || s_heldRateY != 0.0f)){
			// Dotted with the rate being held rather than with an axis, so a diagonal
			// turn is judged on the turn and not on one component of it.  Positive
			// means the hand is still travelling the way the camera is already going:
			// a sweep that simply ran off the end of the bar, so the hold is refreshed
			// rather than allowed to expire under a player who is still aiming.
			if(swingX*s_heldRateX + swingY*s_heldRateY > 0.0f){
				s_heldSeconds = 0.0f;
			}else{
				// Swept back the other way.  The aim is on its way back onto the screen
				// and the camera should wait for it rather than guess which way it was
				// heading; the crosshair is left where it is, and the hold below
				// returns it to rest if the bar never comes back.
				s_heldRateX = 0.0f;
				s_heldRateY = 0.0f;
			}
		}else{
			// Not sweeping, so this is a hand that has stopped or a remote that has
			// been put down: keep the last rate for a moment in case the bar is
			// reacquired, then give the crosshair back to rest rather than sit in a
			// corner for a remote nobody is holding.
			//
			// First time through, the reticle is picked up where the bar was lost.
			if(!s_lostActive){
				s_lostActive = true;
				s_lostX = s_lastTargetX;
				s_lostY = s_lastTargetY;
			}
			s_heldSeconds += s_pointerDt;
			if(s_heldSeconds >= kPointerHoldSeconds){
				stopPointerHold();
				if(aimWithPointer)
					steerCrosshair(kAimDefaultX, kAimDefaultY);
			}
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

