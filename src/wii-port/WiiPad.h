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
// The PAD_COOP block is couch co-op's partners: whichever controllers are
// present besides player 1's.  Capturing them is also what tells CCoop a
// controller exists, so it is done every frame.  Any other padID reads nothing.
void WiiPadCapture(int padID, CControllerState &state);

// The Nunchuk's lean, in the right stick's units (-128..128), when the ped the
// pad drives is in a helicopter, the Rhino or the fire truck -- the vehicles
// where that axis is the yaw, the turret or the hose.  Zero in anything else,
// or with no Nunchuk, so the D-pad and any right stick keep working exactly as
// they did.
s16 WiiNunchukTiltSteering(int padID);

// The same, forward and back, for the fire truck's hose elevation -- the one
// other axis its stick does not drive.  Zero everywhere else.
s16 WiiNunchukTiltPitch(int padID);

// The Wiimote pointer, as the mouse the engine already knows how to use.  In
// game it reports a turn rate as relative motion; in a menu it drives the
// cursor's absolute position, which it writes straight to the frontend.
// Ignored while player 1 is on a GameCube pad.  Player 1's only: the second
// player's pointer is a reticle and nothing else, and WiiPadCapture reads it.
void WiiPadCaptureMouse(CMouseControllerState &state);

// Drives the rumble motors from the shake the game asked for, and spends down
// its remaining duration.  No other backend on this platform does either.
// A Wii Remote's motor is written by its channel and a GameCube pad's by its
// port, never one for the other; a partner's only buzzes while they are in the
// game; and a controller that stops being a player's is told to stop once.
void WiiPadUpdateRumble(void);

// The WPAD channel of player 1's Wii Remote, or -1 when player 1 is on a
// GameCube pad.  Player 1 is the first remote to connect, not channel 0.
s32 WiiPadLeadChannel(void);

// The remote the speaker should drive: player 1's, or the first partner remote
// when player 1 is on a GameCube pad.  -1 when there is no Wii Remote at all.
s32 WiiPadSpeakerChannel(void);

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

// True while the remote the speaker drives is in the hands of couch co-op's
// second player.  Its sounds are player 1's, so clips stay silent there; a
// phone line may still use it when it is the only remote in the room.
bool WiiPadRemoteIsPartners(void);

// What a Wii Remote's POWER button should do, asked from its callback on that
// remote's channel.  True when it is the console's way out: player 1's remote,
// or any remote while nobody has a controller of their own yet.  False for
// anybody else's, which is theirs to switch off; WiiPadScan then does it, so
// the game goes on.
bool WiiPadPowerButtonQuits(s32 chan);

// The physical button a prompt asks for, named as it is printed on the
// player's own controller: "A" / "B" / "1" / "2" / "Y" / "X" / "ZL" / "R" /
// "D-PAD DOWN".  The actions are the pad fields the engine reads -- Square
// jumps, LeftShock ducks on foot, Triangle is the nod -- so one prompt serves
// every pad: the button named changes (Triangle is 2, X or Y) while the action
// does not.  Nil when that player has no controller, and the caller falls back
// to its own wording.
enum WiiPadPrompt
{
	WII_PROMPT_JUMP,	// Square
	WII_PROMPT_DUCK,	// LeftShock on foot
	WII_PROMPT_NOD,		// Triangle
	WII_PROMPT_FIRE,	// Circle
	WII_PROMPT_ENTER,	// Cross
	WII_PROMPT_COUNT
};
const char *WiiPadPromptButton(int player, WiiPadPrompt prompt);

// The kind of controller a player is holding, named the way WiiPadQueryPrimary
// names player 1's: WII_PAD_NONE when they have none, and a Wii Remote reads as
// a Classic Controller or a Nunchuk when one is plugged in.  For wording that
// has to name a button, where the buttons differ by controller.
WiiConnectedPad WiiPadQueryPlayer(int player);

// Whether the Wii Remote a player is holding reports its batteries as low.  The
// remote sets the flag in its status report, which it sends when it connects and
// when something is plugged into it, so this can lag a battery that is running
// out.  False for a GameCube pad and for anybody without a controller.
bool WiiPadBatteryLow(int player);

// A short buzz in a player's hands, for the moment their controller joins the
// game.  Independent of whether their character is in the world yet, which the
// ordinary rumble of a partner's controller waits for.
void WiiPadJoinPulse(int player);

#endif
