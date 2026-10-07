#ifndef WIIPAD_H
#define WIIPAD_H

#include <gccore.h>

class CControllerState;
class CMouseControllerState;

enum WiiConnectedPad
{
	WII_PAD_NONE = 0,
	WII_PAD_GAMECUBE,
	WII_PAD_CLASSIC,          // Classic Controller and Classic Controller Pro
	WII_PAD_WIIMOTE_NUNCHUK,
	WII_PAD_WIIMOTE
};

// Brings up both input stacks.  Call once, before the first capture.
//
// The size is the one the pointer reports against, and it is passed in rather
// than read from RsGlobal because this runs before psInitialize fills that in.
// It has to be the same size psInitialize goes on to store, or every offset the
// pointer is measured against the centre of the screen by is wrong.
void WiiPadInitialise(int pointerWidth, int pointerHeight);

// Wii stick tuning, re-asserted after boot and after LoadSettings so a PC
// gta_vc.set cannot leave a huge deadzone behind.  The control method itself
// (Classic lock-on or Standard free aim) is the player's choice and is not
// touched here.
void WiiPadApplyControlDefaults(void);

// Latches one sample from both stacks.  Everything below reads whatever this
// last left behind, so it has to run first and exactly once per frame.
//
// It is separate from WiiPadCapture because CPad::UpdatePads calls UpdateMouse()
// before CapturePad(), so the pointer would otherwise be read from the previous
// frame's sample.  Scanning inside either one of them would also scan twice on a
// frame where both run, and both stacks latch per scan: the second scan drops
// whatever the first had not been read yet.
void WiiPadScan(void);

// Reads one player's controller into a pad state, so the game never has to know
// which kind they picked up.
//
// padID 0 is player 1: a GameCube pad in port 1 if there is one, which then
// owns the slot and silences the first Wii Remote, and otherwise that remote.
// padID PAD_COOP is couch co-op's second player: whichever controller is
// present besides player 1's.  Capturing it is also what tells CCoop a second
// controller exists, so it is done every frame.  Any other padID reads nothing.
void WiiPadCapture(int padID, CControllerState &state);

// The Nunchuk's lean, in the right stick's units (-128..128), when the ped the
// pad drives is in a helicopter or the Rhino -- the two vehicles where that
// axis is the yaw and the turret.  Zero in anything else, or with no Nunchuk,
// so the D-pad and any right stick keep working exactly as they did.
s16 WiiNunchukTiltSteering(int padID);

// The Wiimote pointer, as the mouse the engine already knows how to use.  In
// game it reports a turn rate as relative motion; in a menu it drives the
// cursor's absolute position, which it writes straight to the frontend.
// Ignored while player 1 is on a GameCube pad.  Player 1's only: the second
// player's pointer is a reticle and nothing else, and WiiPadCapture reads it.
void WiiPadCaptureMouse(CMouseControllerState &state);

// Drives the rumble motors from the shake the game asked for, and spends down
// its remaining duration.  No other backend on this platform does either.
void WiiPadUpdateRumble(void);

// Which controller currently owns pad 0 (GameCube preferred).
WiiConnectedPad WiiPadQueryPrimary(void);
const char *WiiPadPrimaryName(WiiConnectedPad pad);

// True once something that can actually play is connected: a GameCube pad, a
// Classic Controller, or a Wiimote with a Nunchuk.  A bare Wiimote is not enough
// (nothing on it walks or steers), so the boot screen blocks on this.
bool WiiPadCanPlay(void);

// True once HOME has asked to go back to the Wii menu.  The request quits the
// game the ordinary way; main() checks this after the log is closed and makes
// the SYS_ResetSystem call there.
bool WiiPadReturnToMenuRequested(void);

// True while the Wiimote on channel 0 is in the hands of couch co-op's second
// player.  The remote speaker is that remote's, and its sounds are player 1's.
bool WiiPadRemoteIsPartners(void);

#endif
