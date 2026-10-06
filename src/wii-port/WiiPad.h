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

// Merges every controller bound to this pad index into a single state, so the
// game never has to know which one the player picked up.  A connected GameCube
// pad on this channel takes exclusive ownership and silences Wiimote/Classic.
void WiiPadCapture(int padID, CControllerState &state);

// The Wiimote pointer, as the mouse the engine already knows how to use.  In
// game it reports a turn rate as relative motion; in a menu it drives the
// cursor's absolute position, which it writes straight to the frontend.
// Ignored while a GameCube pad is active on channel 0.
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

#endif
