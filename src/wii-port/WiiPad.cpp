#include <cmath>
#include <cstdio>

#include <gccore.h>
#include <ogc/consol.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include "common.h"
#include "Camera.h"
#include "Coop.h"
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

// The same delta pitches further than it yaws: Cam.cpp scales the vertical one
// by 4.0*m_fMouseAccelVertical against 2.5*m_fMouseAccelHorzntl for the
// horizontal, and m_fMouseAccelVertical is m_fMouseAccelHorzntl + 0.0005, so at
// the default settings pitch comes out about twice as fast as yaw.  This takes
// most of that back out but leaves the pitch still a little under the yaw, the
// way the stick path's own 0.6 factor in Cam.cpp does.  0.26 is the balance now
// that kPointerSaturation lets both axes reach full rate; it was 0.32 while the
// vertical axis topped out short of that, and 0.5 for one build that mistook an
// off-centre aim box (see kAimBoxes) for a slow pitch.
constexpr float kPointerPitchScale = 0.26f;
// The same balance for the car camera.  Process_FollowCar_SA applies one gain to both
// axes where the on-foot camera pitches about twice as fast as it yaws, so nothing has
// to be taken back out first: this is simply the ratio the factor above ends up at.
constexpr float kPointerPitchScaleCar = 0.5f;

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

// --- which controller is which player ----------------------------------------
// Worked out once per scan.  Player 1's rule is the one this backend has always
// had: a GameCube pad in port 1 owns the slot, and otherwise it is the first
// Wii Remote.  The partners -- couch co-op, CPad's PAD_COOP block -- are
// whichever controllers are there besides: see resolveDevices.
struct PadDevice
{
	enum Kind { NONE, GAMECUBE, WIIMOTE };
	Kind kind;
	int channel;
};
enum { PLAYER_ONE = 0, PLAYER_TWO, PLAYER_THREE, PLAYER_FOUR, NUM_PLAYERS };
PadDevice s_devices[NUM_PLAYERS];

// Which player a pad slot belongs to: pad 0 is player 1, and the co-op slots
// follow in order -- PAD_COOP is the first partner, PAD_COOP2 the second.
// -1 for anything else (PAD2 is the engine's debug pad and is nobody's).
int
playerForPad(int padID)
{
	if(padID == 0)
		return PLAYER_ONE;
	if(padID >= PAD_COOP && padID < MAX_PADS)
		return PLAYER_TWO + (padID - PAD_COOP);
	return -1;
}

// The inverse, for the places that walk the players.
int
padForPlayer(int player)
{
	return player == PLAYER_ONE ? 0 : PAD_COOP + (player - PLAYER_TWO);
}

// --- Nunchuk flick-down jump -------------------------------------------------
// WiiPadScan measures the gesture and raises the player's pulse for exactly one
// frame; captureWiimote folds that into Square, the field JumpJustDown reads.
// A flick is a jolt in the Nunchuk's acceleration measured against the gravity
// it is already carrying, as a fraction of g.  Measuring the vector magnitude
// rather than one axis's per-frame delta is what makes it work in practice: a
// delta between two consecutive scans misses the peak whenever a scan lands
// between the flick and its return, which is why the old version felt janky and
// easy to miss.  The readings are libogc's calibrated g-forces (nunchuk.gforce)
// and not the raw counts beside them: those sit on a zero offset of about 512 an
// axis, and measured against that offset no flick can reach the threshold.  The
// settle window turns the rebound into "not armed yet" instead of a second,
// phantom jump.
//
// One detector per player, because each one is following the gravity its own
// Nunchuk is carrying: two remotes fed through one would read every sample as
// a jolt against the other hand's baseline.
struct FlickDetector
{
	bool  pulse;          // the one-frame result
	float gravity;        // slow |accel| at rest, about one g
	float gx, gy, gz;     // slow gravity vector: which way is down
	bool  settling;       // deaf while a flick rings down
	float settleT;
};
static FlickDetector s_flick[NUM_PLAYERS];
static const float kFlickGravityFollow = 0.04f;  // slow baseline follow, per scan
static const float kFlickFraction = 0.65f;       // a jolt must exceed ~65% of g: a decisive flick only
static const float kFlickRearmFraction = 0.35f;  // "settled" below ~35% of g
static const float kFlickSettleSec = 0.09f;      // stay deaf this long after a flick
static const float kFlickDownAlign = 0.55f;      // must still point into gravity: down-flick only
static const float kFlickMaxSaneG = 8.0f;        // past this the reading is not a reading

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
// The box while the aim button is held.  One box cannot do both of its jobs: getting
// around wants the camera to start turning early, and lining up a shot wants it not to
// turn at all, because every small correction that crosses the edge drags the view
// along with it.  So holding Z opens the box out almost to the screen edge -- the view
// holds still and only the crosshair moves -- and letting go brings the player's own
// box back.  The view can still be turned with Z held, slowly, by pointing off the
// edge of the screen.
//
// Eased between the two rather than switched.  The pointer is often outside the small
// box and inside this one at the moment Z is released, and a switch would start the
// camera turning at whatever rate that distance asks for, all at once.
constexpr AimBox kAimBoxPrecise = { 0.06f, 0.94f, 0.06f, 0.94f };
constexpr float kAimPreciseTau = 0.12f;
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
// Whether player 1 aims with the pointer at all.  Not on a Classic Controller:
// that hangs off a remote lying on the sofa, and its player aims with a stick.
bool s_leadPointerAims = true;

// The turn rate the camera is actually using, eased towards whatever the response
// curve asks for.  Shared by both pointer paths -- the aiming one and the plain
// rate camera -- because they are never active at the same time and because the
// two are meant to feel like the same instrument.  See applyTurnSpinUp.
float s_turnRate;

// How far the aim box has opened towards kAimBoxPrecise, 0 to 1.
float s_precise;

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
// The carry-on itself: a velocity seeded from the above and left to bleed away,
// and the virtual position it has produced, clamped to the screen.
static float s_lostVelX;
static float s_lostVelY;
static float s_lostX;
static float s_lostY;
static bool s_lostActive;

// How fast that carried-on velocity bleeds away.  Per second, so a sweep coasts
// to a stop over a few frames instead of running at the edge forever.
static const float kLostCoastDecay = 0.35f;

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

// The ped a pad drives, which is not always the one in focus any more.
CPlayerPed *
padPlayer(int padID)
{
	const int player = playerForPad(padID);
	if(player < 0)
		return nil;
	return CCoop::GetPlayerPed(player);
}

bool
captureGameCube(int channel, uint32 connectedMask, CControllerState &state,
	StickAccumulator &sticks, const StickSettings &settings)
{
	if((connectedMask & (1 << channel)) == 0)
		return false;

	const u16 buttons = PAD_ButtonsHeld(channel);

	// Face buttons: A=Cross, B=Circle, X=Square, Y=Triangle.  In v3, A is the gas
	// (Cross, the "commit" button) and B the fire (Circle, the "shoot" button).
	// Same field layout the Wiimote gets, so the Mode 0 rebinds read the same
	// whichever pad is in hand.
	setButton(state.Circle, buttons & PAD_BUTTON_B);   // B: fire (foot) / fire+drive-by (car)
	setButton(state.Cross, buttons & PAD_BUTTON_A);    // A: enter+sprint (foot) / gas (car)
	setButton(state.Square, buttons & PAD_BUTTON_X);   // X: jump
	setButton(state.Triangle, buttons & PAD_BUTTON_Y); // Y: exit vehicle
	setButton(state.Start, buttons & PAD_BUTTON_START);

	// L is the brake (RightShoulder1), Z the horn (LeftShoulder1), R the radio --
	// and, on foot, crouch, which reads the same field (CPad::DuckJustDown).
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

	// Same v3 field layout as the Wiimote and the GameCube pad, so the Mode 0
	// rebinds read the same whichever pad is in hand.  Physical A is the gas
	// (Cross, the "commit" button) and B the fire (Circle, the "shoot" button),
	// L the brake (RightShoulder1), R the horn (LeftShoulder1), ZL the radio and,
	// on foot, crouch (both read LeftShock, as the GameCube pad's R does), and the
	// D-pad the vehicle cluster when it has two sticks of its own to walk with.
	setButton(state.Cross, buttons & WPAD_CLASSIC_BUTTON_A);    // A: enter+sprint (foot) / gas (car)
	setButton(state.Circle, buttons & WPAD_CLASSIC_BUTTON_B);   // B: fire (foot) / fire+drive-by (car)
	setButton(state.Square, buttons & WPAD_CLASSIC_BUTTON_Y);   // Y: jump
	setButton(state.Triangle, buttons & WPAD_CLASSIC_BUTTON_X); // X: exit vehicle
	setButton(state.RightShoulder1, buttons & WPAD_CLASSIC_BUTTON_FULL_L); // L: brake
	setButton(state.LeftShoulder1, buttons & WPAD_CLASSIC_BUTTON_FULL_R);  // R: horn
	setButton(state.LeftShock, buttons & WPAD_CLASSIC_BUTTON_ZL);          // ZL: radio (car) / crouch (foot)
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
captureWiimote(int padID, const WPADData &data, u32 expansion, CControllerState &state,
	StickAccumulator &sticks, const StickSettings &settings)
{
	const u32 buttons = data.btns_h;
	const bool hasNunchuk = expansion == WPAD_EXP_NUNCHUK;
	// This pad's own player: 1 is jump on foot and the radio in a car, and with
	// two players one of them can be in a car while the other is not.
	CPlayerPed *player = padPlayer(padID);
	const bool inCar = player != nil && player->bInVehicle;
	const bool flickPulse = s_flick[playerForPad(padID)].pulse;

	const bool dpadLeft = (buttons & WPAD_BUTTON_LEFT) != 0;
	const bool dpadRight = (buttons & WPAD_BUTTON_RIGHT) != 0;
	const bool dpadDown = (buttons & WPAD_BUTTON_DOWN) != 0;
	const bool dpadUp = (buttons & WPAD_BUTTON_UP) != 0;

	// v3 layout, Wiimote + Nunchuk:
	//   A  enter vehicle / sprint (foot)   accelerate (car)      <- the "commit" button
	//   B  fire (foot)                    fire, drive-by, car gun (car)  <- "shoot"
	//   Z  -- (the pointer aims)         brake and reverse (car)
	//   1  jump (foot)                    radio (car)
	//   2  --                             exit vehicle (car)
	//   C  --                             horn (car)
	//   D-pad   cycle weapon / look behind / crouch (foot)
	//           handbrake / look L-R / look behind (car)
	//   +  pause (always)
	//   -  camera mode (foot)
	//   flick  jump; HOME is the system menu, not the game
	// The Nunchuk is required: the stick does all movement, so without one there is
	// nothing to walk or steer with.  Rather than pretend a bare Wiimote works, the
	// D-pad is always the cluster in-game and the boot screen blocks until a
	// Nunchuk is plugged in (or a Classic Controller or GameCube pad, which bring
	// sticks of their own, takes over).  In the menus the D-pad goes back to being
	// a D-pad.
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
	setButton(state.Square, flickPulse || jumpByOne);

	// The radio and crouch read the same field, LeftShock (ChangeStationJustDown
	// and DuckJustDown), so the field follows the context: 1 in a car, where it is
	// the radio, and D-pad down on foot, where it crouches.  D-pad down is the
	// handbrake in a car and nothing else on foot -- walking and the scope no
	// longer read it -- and 1 on foot is jump.  1 used to set this field on foot as
	// well, so it crouched whenever a jump could not start (a heavy weapon out,
	// mid-attack), and the Wiimote had no crouch of its own.  Sniper scope entry
	// already comes from the aim button (Z, which feeds RightShoulder1), so 1 is not
	// needed for that either.
	setButton(state.LeftShock, inCar ? (buttons & WPAD_BUTTON_1) != 0 : (dpadIsCluster && dpadDown));
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

// Shared turn-rate response.  `magnitude` is how far the pointer sits from the
// aim point (the screen centre, or the edge of the aim box), in the normalised
// half-height units the callers use.  The response is LINEAR in how far outside
// the dead zone that is, up to kPointerSaturation; the note in the body says why.
// Once the pointer is off the sensor the rate is capped at
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
	// Linear, and settled by measurement rather than by argument.  This was a
	// player-selectable curve for one build, offering linear, quadratic and a
	// blend; the answer on real hardware was that SMALL box with LINEAR is what
	// feels best, so the setting is gone and this is the curve.
	//
	// Which is the more interesting half of that answer.  SMALL wins because a
	// smaller box leaves more travel outside it for the ramp to use: half-extent
	// 0.19 instead of 0.27, so with the halved dead zone only 0.30 of the travel is
	// dead rather than 0.49.  LINEAR wins because a constant slope means an equal
	// move always turns the same amount, and that predictability is worth more
	// than the fine placement the curved ramps bought -- at least with a pointer,
	// where the reticle itself already provides the precision and the camera does
	// not need to.
	//
	// The curves are one expression away if that ever stops being true: t*t for
	// fine-at-the-edge, t*t*(2.0f - t) to keep the flat start and recover the
	// mid-range.
	return kPointerRatePerSec*t;
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
	const float rate = applyTurnSpinUp(pointerTurnRate(magnitude, pointerOffScreen(data)));
	if(rate <= 0.0f)
		return false;

	// The unit direction comes from the raw vector, so only the SPEED goes
	// through the response.  Curving each axis on its own would bend diagonals
	// towards the nearer axis, the same directional error the normalisation
	// above avoids.
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
// crosshair the HUD shows (the same test as Hud.cpp), in the Standard method, and
// the option on.  Everything else keeps the fixed crosshair and the rate camera.
bool
pointerAimWanted(void)
{
	if(!WiiPointerAimEnabled || !CCamera::m_bUseMouse3rdPerson)
		return false;
	CPlayerPed *player = FindPlayerPed();
	// Getting in or out of a car is excluded because the camera is mid-hand-over
	// there.  Driving sits behind its own switch, AIM IN CAR: with it on the camera
	// code gives vehicles the one car camera that reads the pointer (see
	// WiiPointerAimInCar and CCamera::UseFreeCarCam), and with it off the stock car
	// camera never looks at the pointer at all.
	if(player == nullptr ||
	   player->m_nPedState == PED_ENTER_CAR || player->m_nPedState == PED_CARJACK)
		return false;
	if(player->bInVehicle && !WiiAimInCar)
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
	// So the next time the pointer takes the crosshair it starts from a standstill
	// and eases up, rather than inheriting whatever speed it was released at.  On
	// the hand-over only: this runs every frame the pointer is NOT aiming, which is
	// exactly when the plain rate camera is using s_turnRate, and zeroing it here
	// each frame held that camera to the first step of its spin-up for good.
	if(s_aimActive)
		s_turnRate = 0.0f;
	s_aimActive = false;
	s_precise = 0.0f;
	CCamera::m_f3rdPersonCHairMultX = kAimDefaultX;
	CCamera::m_f3rdPersonCHairMultY = kAimDefaultY;
}

// Moves a reticle toward a target position through the jitter filter.  It
// starts on the target when the pointer first takes it over, rather than gliding
// there from wherever it was left.
void
smoothReticle(float &aimX, float &aimY, bool &active, float targetX, float targetY)
{
	if(!active){
		aimX = targetX;
		aimY = targetY;
		active = true;
	}else{
		const float errorX = targetX - aimX;
		const float errorY = targetY - aimY;
		const float error = std::sqrt(errorX*errorX + errorY*errorY);
		const float tau = kAimSmoothTau/(1.0f + kAimSmoothGain*error);
		const float follow = 1.0f - std::exp(-s_pointerDt/tau);
		aimX += errorX*follow;
		aimY += errorY*follow;
	}
}

// Player 1's reticle is the camera's crosshair: the HUD draws at it and, out of
// couch co-op, the shot is traced through it.
void
steerCrosshair(float targetX, float targetY)
{
	smoothReticle(s_aimX, s_aimY, s_aimActive, targetX, targetY);
	CCamera::m_f3rdPersonCHairMultX = s_aimX;
	CCamera::m_f3rdPersonCHairMultY = s_aimY;
	// In couch co-op each player aims at their own reticle rather than along
	// the camera, and this is how CCoop learns where player 1's is.  Reported
	// only from here, so a pointer nobody is aiming with goes quiet and CCoop
	// lets that player's aim lapse.  Nor from the remote behind a Classic
	// Controller: a glimpse of the sensor bar from the sofa cushions would
	// outrank the stick its player is aiming with.
	if(s_leadPointerAims)
		CCoop::ReportPointer(PLAYER_ONE, s_aimX, s_aimY);
}

// Each partner's reticle.  There is no camera for it to turn and no crosshair
// of the engine's for it to be, so it is only ever this: where their remote is
// pointing, smoothed, handed to CCoop.
float s_partnerAimX[NUM_PLAYERS];
float s_partnerAimY[NUM_PLAYERS];
bool s_partnerAimActive[NUM_PLAYERS];
float s_partnerAimLost[NUM_PLAYERS];

void
capturePartnerPointer(int player, const WPADData &data, u32 expansion)
{
	const float width = (float)RsGlobal.maximumWidth;
	const float height = (float)RsGlobal.maximumHeight;
	// A Classic Controller hangs off a remote that is lying on the sofa; its
	// player aims with the right stick, which CCoop reads off the pad itself.
	if(!WiiPointerAimEnabled || !CCoop::PairActive() || FrontEndMenuManager.m_bMenuActive ||
	   expansion == WPAD_EXP_CLASSIC || width <= 0.0f || height <= 0.0f){
		s_partnerAimActive[player] = false;
		return;
	}
	if(!data.ir.valid){
		// Off the sensor bar.  Say nothing, and after a moment forget where it
		// was, so that coming back is a jump to the new place and not a glide
		// across the screen from the old one.
		s_partnerAimLost[player] += s_pointerDt;
		if(s_partnerAimLost[player] > 0.25f)
			s_partnerAimActive[player] = false;
		return;
	}
	s_partnerAimLost[player] = 0.0f;
	float x = data.ir.x/width;
	float y = data.ir.y/height;
	x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
	y = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
	smoothReticle(s_partnerAimX[player], s_partnerAimY[player], s_partnerAimActive[player], x, y);
	CCoop::ReportPointer(player, s_partnerAimX[player], s_partnerAimY[player]);
}

// A Wii Remote that is connected and reporting.
bool
wiimoteReady(int channel, u32 &expansion)
{
	WPADData *data = WPAD_Data(channel);
	if(data == nullptr || data->err != WPAD_ERR_NONE)
		return false;
	expansion = probeExpansion(channel, *data);
	return true;
}

void
resolveDevices(void)
{
	for(int player = 0; player < NUM_PLAYERS; player++){
		s_devices[player].kind = PadDevice::NONE;
		s_devices[player].channel = 0;
	}

	// Player 1 first: the GameCube pad in port 1, or the first Wii Remote.
	if(s_connectedGameCubePads & 1){
		s_devices[PLAYER_ONE].kind = PadDevice::GAMECUBE;
		s_devices[PLAYER_ONE].channel = 0;
	}else{
		s_devices[PLAYER_ONE].kind = PadDevice::WIIMOTE;
		s_devices[PLAYER_ONE].channel = WPAD_CHAN_0;
	}

	// Then everyone else: the remaining GameCube pads in port order, then the
	// Wii Remotes player 1 is not holding.  A remote only counts with something
	// to walk with plugged into it -- the same rule the boot screen holds
	// player 1 to -- which also means a remote left on the table does not put a
	// player in the game.
	int next = PLAYER_TWO;
	for(int port = 1; port < 4 && next < NUM_PLAYERS; port++){
		if(s_connectedGameCubePads & (1 << port)){
			s_devices[next].kind = PadDevice::GAMECUBE;
			s_devices[next].channel = port;
			next++;
		}
	}
	for(int channel = 0; channel < WPAD_MAX_WIIMOTES && next < NUM_PLAYERS; channel++){
		if(s_devices[PLAYER_ONE].kind == PadDevice::WIIMOTE &&
		   channel == s_devices[PLAYER_ONE].channel)
			continue;
		u32 expansion = WPAD_EXP_NONE;
		if(wiimoteReady(channel, expansion) &&
		   (expansion == WPAD_EXP_NUNCHUK || expansion == WPAD_EXP_CLASSIC)){
			s_devices[next].kind = PadDevice::WIIMOTE;
			s_devices[next].channel = channel;
			next++;
		}
	}
}

// One player's flick, measured once per scan.  It fires on a jolt in the
// acceleration vector measured against the gravity the Nunchuk is already
// carrying -- see the constants above.  Three things keep it honest:
//   - it is a transient jolt above a live gravity baseline, not a tilt angle,
//     so holding the remote still never reads as a flick;
//   - the threshold is a fraction of g, so it does not care how the pad's raw
//     accel happens to be scaled or how the remote is held;
//   - a settle window makes the rebound at the end of a flick re-arm the
//     detector rather than fire a second, phantom jump.
// It is on foot only and idle while the stick is deflected, because steering
// and running shake the Nunchuk constantly.
void
updateFlick(FlickDetector &flick, const WPADData *wd, CPlayerPed *ped)
{
	flick.pulse = false;

	const bool nunchukReady = wd != nullptr && wd->err == WPAD_ERR_NONE &&
		wd->exp.type == WPAD_EXP_NUNCHUK;
	const bool onFoot = ped != nullptr && !ped->bInVehicle;
	// Menus run with the player ped still alive on foot, so without this the
	// flick would pulse Square behind an open menu.  Gameplay input is paused
	// in menus anyway, so the pulse would go nowhere -- this just keeps the
	// gesture honest.
	const bool inMenu = FrontEndMenuManager.m_bMenuActive;

	// Calibrated g-forces, not the raw counts in nunchuk.accel: see the note on
	// the constants above.  A Nunchuk whose calibration never arrived reports
	// nonsense here instead (the conversion divides by it), so a reading no real
	// hand could produce is treated as no reading at all; the comparison is
	// written so that a NaN fails it too.
	float ax = 0.0f, ay = 0.0f, az = 0.0f, mag = 0.0f;
	bool readable = false;
	if(nunchukReady && onFoot && !inMenu){
		ax = wd->exp.nunchuk.gforce.x;
		ay = wd->exp.nunchuk.gforce.y;
		az = wd->exp.nunchuk.gforce.z;
		mag = std::sqrt(ax*ax + ay*ay + az*az);
		readable = mag < kFlickMaxSaneG;
	}
	if(!readable){
		flick.settling = false;
		flick.settleT = 0.0f;
		return;
	}

	if(flick.gravity <= 0.0f){
		flick.gravity = mag;                    // first sample seeds the baseline
		flick.gx = ax; flick.gy = ay; flick.gz = az;
	}else{
		flick.gravity += (mag - flick.gravity)*kFlickGravityFollow;
		flick.gx += (ax - flick.gx)*kFlickGravityFollow;
		flick.gy += (ay - flick.gy)*kFlickGravityFollow;
		flick.gz += (az - flick.gz)*kFlickGravityFollow;
	}
	// How far the current acceleration sits above the gravity baseline, as a
	// fraction of that baseline.  A deliberate flick spikes this; holding,
	// walking, or steering do not.
	const float dev = (mag - flick.gravity)/(flick.gravity > 0.1f ? flick.gravity : 1.0f);
	// Which way is "down" right now, from the slowly-followed gravity
	// vector (not the raw sample, which is what the jolt perturbs).  At rest
	// the accelerometer already reads the gravity vector, so this alignment
	// is about 1 however the remote is held.  A down-flick pushes further
	// along gravity and keeps it near 1; an up-flick opposes gravity and
	// drops it, so requiring real alignment rejects up-flicks.  A cosine over
	// all three axes, so it is the angle that is tested and not the size of
	// the jolt.
	const float glen = std::sqrt(flick.gx*flick.gx + flick.gy*flick.gy + flick.gz*flick.gz);
	const float align = (glen > 0.1f && mag > 0.1f) ?
		(ax*flick.gx + ay*flick.gy + az*flick.gz)/(glen*mag) : 1.0f;
	const bool wentDown = align > kFlickDownAlign;
	if(flick.settling){
		// Wait for the jolt and its rebound to fall away before arming again,
		// so one flick is exactly one pulse.
		if(dev > kFlickRearmFraction)
			flick.settleT = 0.0f;
		else
			flick.settleT += s_pointerDt;
		if(flick.settleT >= kFlickSettleSec)
			flick.settling = false;
	}else if(dev > kFlickFraction && wentDown){
		flick.pulse = true;
		flick.settling = true;
		flick.settleT = 0.0f;
	}
}

// The pointer's two jobs while it owns the crosshair.  The crosshair tracks the
// pointer across the WHOLE screen (so you can aim anywhere, not just a small
// centre box), and the camera turns only once the pointer is pushed past the
// screen edge.  The WII_BOX option now controls how far in from that edge the
// turn begins: a smaller box turns sooner, a larger one keeps the camera still
// nearer the edge.  Holding the aim button opens it out further still (see
// kAimBoxPrecise).  The turn is linear in how far outside the edge the pointer
// is and is capped at half the maximum once the pointer is off the sensor.
// Returns false when the pointer is inside the turn ring (pure aiming, camera
// still), which is what leaves the camera steady while the player aims.
bool
irAimRate(const WPADData &data, float &outCrosshairX, float &outCrosshairY,
	float &outX, float &outY, float pitchScale)
{
	const float width = (float)RsGlobal.maximumWidth;
	const float height = (float)RsGlobal.maximumHeight;
	if(width <= 0.0f || height <= 0.0f)
		return false;

	int size = WiiPointerBox;
	if(size < 0 || size >= (int)(sizeof(kAimBoxes)/sizeof(kAimBoxes[0])))
		size = 0;
	const AimBox &loose = kAimBoxes[size];

	// Only where the crosshair is the pointer.  The aim button also holds a scope up,
	// and through a scope the camera IS the aim: opening the box there would leave
	// nothing to turn it with.
	const bool steady = CPad::GetPad(0)->GetTarget() &&
		TheCamera.Cams[TheCamera.ActiveCam].Using3rdPersonMouseCam();
	s_precise += ((steady ? 1.0f : 0.0f) - s_precise)*(1.0f - std::exp(-s_pointerDt/kAimPreciseTau));
	const AimBox box = {
		loose.top + (kAimBoxPrecise.top - loose.top)*s_precise,
		loose.bottom + (kAimBoxPrecise.bottom - loose.bottom)*s_precise,
		loose.left + (kAimBoxPrecise.left - loose.left)*s_precise,
		loose.right + (kAimBoxPrecise.right - loose.right)*s_precise,
	};

	// Couch co-op: the crosshair is a reticle and nothing else.  The camera is
	// shared and takes no input from aiming (CCam::Process_WiiCoop), so there is
	// nothing for a turn to accomplish.  outCrosshair* is set below either way;
	// only the turn is dropped.
	//
	// CCoop::PairActive(), not the menu toggle: the toggle stays on through a
	// mission and while the session waits for a second player, and in both the
	// camera is the ordinary one, which does turn.
	const bool reticleOnly = CCoop::PairActive();

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

	// Not returned early on zero.  The spin-up has to see the zero in order to
	// snap a falling rate to a stop.
	const float applied = applyTurnSpinUp(pointerTurnRate(magnitude, pointerOffScreen(data)));
	// Co-op: the shared camera is fixed and the pointer is an aim point only, so the
	// turn path is bypassed outright rather than fighting a camera that never turns
	// ("the pointer is a reticle only", Gate 0).  The single-player path above is
	// left intact -- it is what a lone player still gets.
	if(applied <= 0.0f || reticleOnly)
		return false;

	outX = (overX/magnitude)*applied;
	outY = (overY/magnitude)*applied*pitchScale;
	return true;
}

} // namespace

int8_t WiiPointerAimEnabled = 1;
int8_t WiiPointerBox = 0;
int8_t WiiAimInCar = 1;
int8_t WiiDriveByAnyWeapon = 0;
// The partner's model, as an index into the list on the co-op page.  0 is "same
// as player 1"; the rest are the special-character models a script will not
// reuse, which is what makes them safe to put on a player ped.
int8_t WiiCoopSkin = 0;

// Set by HOME, read by main() once the game has unwound.  See WiiPadScan.
static bool s_returnToMenu;

bool
WiiPadRemoteIsPartners(void)
{
	// The speaker drives the first remote, so player 1's shots are silent
	// through it while that remote is in a partner's hands.
	for(int player = PLAYER_TWO; player < NUM_PLAYERS; player++){
		if(CCoop::GetPlayerPed(player) != nullptr &&
		   s_devices[player].kind == PadDevice::WIIMOTE &&
		   s_devices[player].channel == WPAD_CHAN_0)
			return true;
	}
	return false;
}

// --- the Nunchuk's lean ------------------------------------------------------
// A Wiimote and Nunchuk have no right stick.  In a helicopter the right stick
// is the yaw, and in the Rhino it is the turret, so the Nunchuk's own lean
// stands in for it there and nowhere else: in a car the same axis is the
// camera's look, the drive-by aim and the hydraulics, and none of those are
// steered by leaning.  The fire truck is the other way round -- its stick
// drives the truck and the hose is what is left -- so there the lean drives
// both halves of the hose: left and right steers it, and leaning the Nunchuk
// away or towards the player raises and lowers it.
//
// The accelerometer carries gravity: leaning the Nunchuk over moves it between
// the down axis (Z, across the face) and the right axis (X, along the short
// side), and leaning it forward or back moves it between Z and Y.  The angle
// between the two is the lean -- the same roll every Nunchuk library derives
// from atan2(x, z) -- and it does not care how tightly the hand holds it.
const float kNunchukLeanDead = 0.14f;	// about 8 degrees: a hand at rest
const float kNunchukLeanFull = 0.61f;	// about 35 degrees: hard over

// axis 0 is left/right (X against Z), 1 is forward/back (Y against Z).
static s16
NunchukLean(int padID, int axis)
{
	const int player = playerForPad(padID);
	if(player < 0)
		return 0;
	const PadDevice &device = s_devices[player];
	if(device.kind != PadDevice::WIIMOTE)
		return 0;
	WPADData *data = WPAD_Data(device.channel);
	if(data == nullptr || data->err != WPAD_ERR_NONE || data->exp.type != WPAD_EXP_NUNCHUK)
		return 0;

	const float g = axis == 0 ? data->exp.nunchuk.gforce.x : data->exp.nunchuk.gforce.y;
	const float lean = std::atan2(g, data->exp.nunchuk.gforce.z);
	float amount = (std::fabs(lean) - kNunchukLeanDead)/(kNunchukLeanFull - kNunchukLeanDead);
	if(amount <= 0.0f)
		return 0;
	if(amount > 1.0f)
		amount = 1.0f;
	return (s16)(lean < 0.0f ? -amount*kAxisFullScale : amount*kAxisFullScale);
}

s16
WiiNunchukTiltSteering(int padID)
{
	CPlayerPed *ped = padPlayer(padID);
	if(ped == nullptr || !ped->bInVehicle || ped->m_pMyVehicle == nullptr)
		return 0;
	CVehicle *vehicle = ped->m_pMyVehicle;
	if(vehicle->GetModelIndex() != MI_RHINO && vehicle->GetModelIndex() != MI_FIRETRUCK &&
	   !vehicle->IsRealHeli())
		return 0;
	return NunchukLean(padID, 0);
}

s16
WiiNunchukTiltPitch(int padID)
{
	CPlayerPed *ped = padPlayer(padID);
	if(ped == nullptr || !ped->bInVehicle || ped->m_pMyVehicle == nullptr)
		return 0;
	if(ped->m_pMyVehicle->GetModelIndex() != MI_FIRETRUCK)
		return 0;
	return NunchukLean(padID, 1);
}

// Outside the anonymous namespace: the boot gate in wii_game.cpp calls these.
bool
WiiPadCanPlay(void)
{
	// The same test, in the same order, that WiiPadCapture uses to pick what
	// drives pad 0.  A GameCube pad owns the slot outright and a Classic
	// Controller has two sticks of its own; a Wiimote needs the Nunchuk's.
	if((s_connectedGameCubePads & (1 << PAD_CHAN0)) != 0)
		return true;
	WPADData *data = WPAD_Data(WPAD_CHAN_0);
	if(data == nullptr || data->err != WPAD_ERR_NONE)
		return false;
	const u32 expansion = probeExpansion(WPAD_CHAN_0, *data);
	return expansion == WPAD_EXP_NUNCHUK || expansion == WPAD_EXP_CLASSIC;
}

bool
WiiPadReturnToMenuRequested(void)
{
	return s_returnToMenu;
}

// The switches pointerAimWanted reads for a vehicle, plus the one thing that stops
// WiiPadCaptureMouse feeding the camera at all: player 1 on a GameCube pad.  (A pad
// in another port is couch co-op's partner and leaves player 1's pointer alone.)
// Settings and a connection mask, so it cannot flicker from frame to frame and swap
// the car camera under the player.
bool
WiiPointerAimInCar(void)
{
	return WiiPointerAimEnabled && WiiAimInCar && CCamera::m_bUseMouse3rdPerson &&
		(s_connectedGameCubePads & (1 << PAD_CHAN0)) == 0;
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
	// Mouse steering stays off on this port, whatever a settings file says.  The row
	// that toggles it is not on the Wii's controls page, so a stored "on" could never
	// be turned back off -- and with it on the pointer steers the car (Automobile.cpp)
	// and the car camera refuses the pointer outright (Cam.cpp's FollowCar_SA).
	CVehicle::m_bDisableMouseSteering = true;
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
	// title screens (nothing at risk).
	//
	// The reset is not made from here.  This only asks for it and raises the same
	// RsGlobal.quit every other exit raises, so the frame finishes and the log is
	// closed before main() makes the call -- a reset from inside the scan cut the
	// frame in half and left debug.log ending mid-file, which is what a crash
	// looks like.  SYS_RETURNTOMENU needs the IOS to allow it; if it refuses,
	// main() simply returns to the loader, so HOME always gets you out.
	// Player 1's HOME only.  The launch remote is player 1's when they are on a
	// Wiimote, but when they are on a GameCube pad channel 0 belongs to the partner,
	// and a partner who can reset the console mid-mission is not a thing.  With no
	// remote of their own player 1 still has the pause menu's Quit.
	const PadDevice &leadDevice = s_devices[PLAYER_ONE];
	if(leadDevice.kind == PadDevice::WIIMOTE &&
	   (WPAD_ButtonsDown(leadDevice.channel) & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME))){
		const bool onQuitScreen = FrontEndMenuManager.m_bMenuActive &&
			FrontEndMenuManager.m_nCurrScreen == MENUPAGE_EXIT;
		if(FrontEndMenuManager.m_bGameNotLoaded || FrontEndMenuManager.m_bQuitPromptRequested || onQuitScreen){
			WiiTraceReport("WII pad: HOME pressed, returning to the Wii menu\n");
			s_returnToMenu = true;
			HandleExit();
		}else{
			WiiTraceReport("WII pad: HOME pressed, asking\n");
			FrontEndMenuManager.RequestQuitPrompt();
		}
	}

	// Who is holding what, for everything else this frame.
	resolveDevices();

	// Nunchuk flick-down = jump (one of two triggers; 1 on foot is the other).
	// The gesture is read here, once per scan, and published as a one-frame Square
	// pulse that captureWiimote folds into the pad state (JumpJustDown reads
	// Square).  One detector per player, each reading its own player's remote: see
	// updateFlick.
	for(int player = 0; player < NUM_PLAYERS; player++){
		updateFlick(s_flick[player],
			s_devices[player].kind == PadDevice::WIIMOTE ? WPAD_Data(s_devices[player].channel) : nullptr,
			padPlayer(padForPlayer(player)));
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
	// Pad 0 is player 1 and the PAD_COOP block is couch co-op's partners.
	// Nothing is ever read into pad 1: it is the engine's debug pad, and a
	// controller on it would be pressing debug hotkeys.  See PAD_COOP in Pad.h.
	const int player = playerForPad(padID);
	if(player < 0)
		return;
	const PadDevice &device = s_devices[player];
	// This capture runs every frame whether or not anyone is playing these
	// slots, and it is how co-op learns that somebody could: a partner joins
	// when a controller turns up here and leaves when it has been gone a while.
	if(player != PLAYER_ONE)
		CCoop::ReportPartnerPad(player - PLAYER_TWO, device.kind != PadDevice::NONE);
	if(device.kind == PadDevice::NONE)
		return;

	const StickSettings settings = currentStickSettings();

	StickAccumulator sticks = { 0.0f, 0.0f, 0.0f, 0.0f };
	// A live GameCube pad owns its slot alone.  Merging Wiimote / Classic on
	// top of it was what made IR and remote buttons steal camera and actions
	// while a GC controller was already plugged in.
	if(device.kind == PadDevice::GAMECUBE){
		captureGameCube(device.channel, s_connectedGameCubePads, state, sticks, settings);
	}else{
		WPADData *data = WPAD_Data(device.channel);
		if(data != nullptr){
			const u32 expansion = probeExpansion(device.channel, *data);
			if(expansion == WPAD_EXP_CLASSIC)
				captureClassic(*data, state, sticks, settings);
			else
				captureWiimote(padID, *data, expansion, state, sticks, settings);
			// Player 1's pointer is read in WiiPadCaptureMouse, because for them
			// it is also the mouse.  A partner's is only ever a reticle.
			if(player != PLAYER_ONE)
				capturePartnerPointer(player, *data, expansion);
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

	// Same exclusivity as WiiPadCapture: player 1 on a GameCube pad silences IR,
	// so a live remote cannot steer the camera while they drive with the pad.
	// (It used to be ANY GameCube pad, which was the same thing until a second
	// pad could mean a second player; now a partner plugging one in would have
	// taken player 1's pointer away.)
	if(s_devices[PLAYER_ONE].kind == PadDevice::GAMECUBE){
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
	s_leadPointerAims = data->exp.type != WPAD_EXP_CLASSIC;

	const bool aimWithPointer = !FrontEndMenuManager.m_bMenuActive && pointerAimWanted();
	if(!aimWithPointer)
		releaseCrosshair();

	// How long the pointer has been off the sensor bar.  Where the view is not
	// shared the reticle is carried along the screen edge for as long as that
	// lasts, because the camera is still being turned by it.  Where it is shared
	// nothing is being turned, and a reticle parked at the bottom of the screen
	// for a player who has put the remote in their lap would keep that player's
	// body facing down the screen -- so there it is carried only briefly, and
	// then left for CCoop to retire.
	static float untrackedSeconds = 0.0f;
	untrackedSeconds = tracked ? 0.0f : untrackedSeconds + s_pointerDt;
	const bool carryReticle = !CCoop::PairActive() || untrackedSeconds < 0.6f;

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
			turning = irAimRate(*data, crosshairX, crosshairY, rateX, rateY,
				playerInVehicle() ? kPointerPitchScaleCar : kPointerPitchScale);
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
		// Off the sensor bar.  The reticle is carried on the way it was already
		// travelling and clamped to the screen, so it parks against the edge it left
		// through: the player can see they have run out of sensor, instead of the
		// aim dying silently while the camera carries on.
		if(!s_lostActive){
			s_lostActive = true;
			s_lostX = s_lastTargetX;
			s_lostY = s_lastTargetY;
			// Seeded from the reticle's own last speed, so it carries on at the rate
			// the player was already seeing.
			s_lostVelX = s_ptrVelX;
			s_lostVelY = s_ptrVelY;
			// Half speed while the bar is out.  That cap used to be unreachable:
			// it only ever applied to out-of-range coordinates, and losing the bar
			// outright is the common case by far.
			s_heldRateX *= kPointerOffScreenMaxFrac;
			s_heldRateY *= kPointerOffScreenMaxFrac;
		}
		// Coast and bleed away, rather than sliding along the edge indefinitely.
		s_lostVelX -= s_lostVelX*kLostCoastDecay*s_pointerDt;
		s_lostVelY -= s_lostVelY*kLostCoastDecay*s_pointerDt;
		s_lostX += s_lostVelX*s_pointerDt;
		s_lostY += s_lostVelY*s_pointerDt;
		if(s_lostX < 0.0f) s_lostX = 0.0f; else if(s_lostX > 1.0f) s_lostX = 1.0f;
		if(s_lostY < 0.0f) s_lostY = 0.0f; else if(s_lostY > 1.0f) s_lostY = 1.0f;

		// The last turn rate is kept for a moment in case the bar is reacquired --
		// as often as not this is just the end of a long turn -- and then the
		// crosshair goes back to rest rather than sit in a corner for a remote
		// nobody is holding.
		s_heldSeconds += s_pointerDt;
		if(s_heldSeconds >= kPointerHoldSeconds){
			stopPointerHold();
			// (Not with the view shared, where the middle of the screen is no
			// resting place: it is roughly where the players are standing.)
			if(aimWithPointer && !CCoop::PairActive())
				steerCrosshair(kAimDefaultX, kAimDefaultY);
		}else if(aimWithPointer && carryReticle)
			steerCrosshair(s_lostX, s_lostY);
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
	// One motor per player, driven by that player's own pad: the engine queues
	// each shake on the pad it belongs to, and player 1's hands have no
	// business buzzing for a partner's.  The phase is per player too, so one
	// player's pulse pattern is not spent by another's.
	constexpr float kRumbleMinDuty = 0.35f;
	static float s_rumblePhase[NUM_PLAYERS];

	for(int player = 0; player < NUM_PLAYERS; player++){
		CPad *pad = CPad::GetPad(padForPlayer(player));

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
		int running = 0;
		if(pad->ShakeFreq != 0){
			s_rumblePhase[player] += kRumbleMinDuty + (1.0f - kRumbleMinDuty)*((float)pad->ShakeFreq/255.0f);
			if(s_rumblePhase[player] >= 1.0f){
				s_rumblePhase[player] -= 1.0f;
				running = 1;
			}
		}else
			s_rumblePhase[player] = 0.0f;

		const PadDevice &device = s_devices[player];
		if(device.kind == PadDevice::NONE)
			continue;
		const bool onGameCube = device.kind == PadDevice::GAMECUBE;
		WPAD_Rumble(device.channel, onGameCube ? 0 : running);
		PAD_ControlMotor(device.channel, (onGameCube && running) ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
	}
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

