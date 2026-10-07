#include "common.h"
#if defined DETECT_JOYSTICK_MENU && defined XINPUT
#include <windows.h>
#include <xinput.h>
#if !defined(PSAPI_VERSION) || (PSAPI_VERSION > 1)
#pragma comment( lib, "Xinput9_1_0.lib" )
#else
#pragma comment( lib, "Xinput.lib" )
#endif
#endif
#include "platform.h"
#include "crossplatform.h"
#include "Renderer.h"
#include "Frontend.h"
#include "Font.h"
#include "Camera.h"
#include "Coop.h"
#include "main.h"
#include "MBlur.h"
#include "postfx.h"
#include "custompipes.h"
#include "RwHelper.h"
#include "Text.h"
#include "Streaming.h"
#include "FileLoader.h"
#include "Collision.h"
#include "ModelInfo.h"
#include "Pad.h"
// For CPed::SwitchDebugDisplay, one of the two cheats that is a class member rather
// than a free function.
#include "Ped.h"
#include "ControllerConfig.h"
#include "DMAudio.h"
#include "IniFile.h"
#include "CarCtrl.h"
#include "Population.h"
#include "AimAssist.h"
#ifdef NINTENDO_WII
#include "WiiPointerAim.h"
#include "WiiSpeaker.h"
#endif

// Menu screens array is at the bottom of the file.

#ifdef PC_MENU

#ifdef CUSTOM_FRONTEND_OPTIONS

#if defined(IMPROVED_VIDEOMODE) && !defined(GTA_HANDHELD)
	#define VIDEOMODE_SELECTOR MENUACTION_CFO_SELECT, "FEM_SCF", { new CCFOSelect((int8*)&FrontEndMenuManager.m_nPrefsWindowed, "VideoMode", "Windowed", screenModes, 2, true, ScreenModeAfterChange, true) }, 0, 0, MENUALIGN_LEFT,
#else
	#define VIDEOMODE_SELECTOR
#endif

#ifdef MULTISAMPLING
	#define MULTISAMPLING_SELECTOR MENUACTION_CFO_DYNAMIC, "FED_AAS", { new CCFODynamic((int8*)&FrontEndMenuManager.m_nPrefsMSAALevel, "Graphics", "MultiSampling", MultiSamplingDraw, MultiSamplingButtonPress) }, 0, 0, MENUALIGN_LEFT,
#else
	#define MULTISAMPLING_SELECTOR
#endif

#ifdef CUTSCENE_BORDERS_SWITCH
	#define CUTSCENE_BORDERS_TOGGLE MENUACTION_CFO_SELECT, "FEM_CSB", { new CCFOSelect((int8 *)&FrontEndMenuManager.m_PrefsCutsceneBorders, "Display", "CutsceneBorders", off_on, 2, false) }, 0, 0, MENUALIGN_LEFT,
#else
	#define CUTSCENE_BORDERS_TOGGLE
#endif

#ifdef FREE_CAM
	#define FREE_CAM_TOGGLE MENUACTION_CFO_SELECT, "FEC_FRC", { new CCFOSelect((int8*)&TheCamera.bFreeCam, "Display", "FreeCam", off_on, 2, false) }, 0, 0, MENUALIGN_LEFT,
#else
	#define FREE_CAM_TOGGLE
#endif

#ifdef PS2_ALPHA_TEST
	#define DUALPASS_SELECTOR MENUACTION_CFO_SELECT, "FEM_2PR", { new CCFOSelect((int8*)&gPS2alphaTest, "Graphics", "PS2AlphaTest", off_on, 2, false) }, 0, 0, MENUALIGN_LEFT,
#else
	#define DUALPASS_SELECTOR 
#endif

#ifdef PED_CAR_DENSITY_SLIDERS
	// 0.2f - 3.4f makes it possible to have 1.0f somewhere inbetween
	#define DENSITY_SLIDERS \
		MENUACTION_CFO_SLIDER, "FEM_PED", { new CCFOSlider(&CIniFile::PedNumberMultiplier, "Display", "PedDensity", 0.2f, 3.4f, PedDensityChange) }, 0, 0, MENUALIGN_LEFT, \
		MENUACTION_CFO_SLIDER, "FEM_CAR", { new CCFOSlider(&CIniFile::CarNumberMultiplier, "Display", "CarDensity", 0.2f, 3.4f, CarDensityChange) }, 0, 0, MENUALIGN_LEFT, 
#else
	#define DENSITY_SLIDERS 
#endif

#ifdef NO_ISLAND_LOADING
	#define ISLAND_LOADING_SELECTOR MENUACTION_CFO_SELECT, "FEM_ISL", { new CCFOSelect((int8*)&FrontEndMenuManager.m_PrefsIslandLoading, "Graphics", "IslandLoading", islandLoadingOpts, ARRAY_SIZE(islandLoadingOpts), true, IslandLoadingAfterChange) }, 0, 0, MENUALIGN_LEFT,
#else
	#define ISLAND_LOADING_SELECTOR 
#endif

#ifdef EXTENDED_COLOURFILTER
	#define POSTFX_SELECTORS \
		MENUACTION_CFO_SELECT, "FED_CLF", { new CCFOSelect((int8*)&CPostFX::EffectSwitch, "Graphics", "ColourFilter", filterNames, ARRAY_SIZE(filterNames), false) }, 0, 0, MENUALIGN_LEFT, \
		MENUACTION_CFO_SELECT, "FED_MBL", { new CCFOSelect((int8*)&CPostFX::MotionBlurOn, "Graphics", "MotionBlur", off_on, 2, false) }, 0, 0, MENUALIGN_LEFT,
#else
	#define POSTFX_SELECTORS
#endif	

#ifdef INVERT_LOOK_FOR_PAD
	#define INVERT_PAD_SELECTOR MENUACTION_CFO_SELECT, "FEC_ILU", { new CCFOSelect((int8*)&CPad::bInvertLook4Pad, "Controller", "InvertPad", off_on, 2, false) }, 0, 0, MENUALIGN_LEFT,
#else
	#define INVERT_PAD_SELECTOR
#endif

#ifdef AIM_ASSIST
	// Centred like the rest of the Enhancements page, which is the only page it is
	// on now.
	#define AIM_ASSIST_TOGGLE MENUACTION_CFO_SELECT, "WII_AIM", { new CCFOSelect((int8*)&CAimAssist::bEnabled, "Controller", "AimAssist", off_on, 2, false) }, 0, 0, MENUALIGN_CENTER,
#else
	#define AIM_ASSIST_TOGGLE
#endif

#ifdef NINTENDO_WII
	// All of the port's own toggles are centred, because the only page they are on
	// now is the Enhancements page, whose rows are all centred -- a page has one
	// alignment, and mixing them leaves some rows sitting out on their own.
	#define POINTER_AIM_TOGGLE MENUACTION_CFO_SELECT, "WII_IRA", { new CCFOSelect((int8*)&WiiPointerAimEnabled, "Controller", "PointerAim", off_on, 2, false) }, 0, 0, MENUALIGN_CENTER,
	#define POINTER_BOX_SELECT MENUACTION_CFO_SELECT, "WII_BOX", { new CCFOSelect((int8*)&WiiPointerBox, "Controller", "PointerBox", pointerBoxSizes, 3, false) }, 0, 0, MENUALIGN_CENTER,
	// Couch co-op.  A CCFOSelect rather than a hand-rolled toggle so it persists
	// through the CUSTOM_FRONTEND_OPTIONS loop in SaveINISettings on its own,
	// with no new menu action to keep in step.  Works with one player present --
	// the shared camera simply has one ped to frame -- which is the point: the
	// mode is meant to be judged before a second remote exists.
	#define COUCH_COOP_TOGGLE MENUACTION_CFO_SELECT, "WII_CC", { new CCFOSelect((int8*)&CCamera::bWiiCoopCamera, "Controller", "CouchCoop", off_on, 2, false) }, 0, 0, MENUALIGN_CENTER,
	// Vibration stays left: it is on the gamepad page, which is left-aligned.
	#define VIBRATION_TOGGLE MENUACTION_CFO_SELECT, "FEC_VIB", { new CCFOSelect((int8*)&FrontEndMenuManager.m_PrefsUseVibration, "Controller", "Vibration", off_on, 2, false, VibrationAfterChange) }, 0, 0, MENUALIGN_LEFT,
	#define POINTER_CAR_TOGGLE MENUACTION_CFO_SELECT, "WII_AIC", { new CCFOSelect((int8*)&WiiAimInCar, "Controller", "AimInCar", off_on, 2, false) }, 0, 0, MENUALIGN_CENTER,
	#define DRIVEBY_WEAPONS_TOGGLE MENUACTION_CFO_SELECT, "WII_DBW", { new CCFOSelect((int8*)&WiiDriveByAnyWeapon, "Controller", "DriveByWeapons", off_on, 2, false) }, 0, 0, MENUALIGN_CENTER,
	#define COOP_SKIN_SELECT MENUACTION_CFO_SELECT, "WII_COS", { new CCFOSelect((int8*)&WiiCoopSkin, "Wii", "CoopSkin", coopSkins, COOP_NUM_SKINS, false, CoopSkinAfterChange) }, 0, 0, MENUALIGN_CENTER,
	#define FRIENDLY_FIRE_TOGGLE MENUACTION_CFO_SELECT, "WII_FF", { new CCFOSelect((int8*)&CoopFriendlyFire, "Wii", "FriendlyFire", off_on, 2, false) }, 0, 0, MENUALIGN_CENTER,
	#define SHARED_WANTED_TOGGLE MENUACTION_CFO_SELECT, "WII_SHW", { new CCFOSelect((int8*)&CoopSharedWanted, "Wii", "SharedWanted", off_on, 2, false) }, 0, 0, MENUALIGN_CENTER,
#else
	#define POINTER_AIM_TOGGLE
	#define POINTER_BOX_SELECT
	#define COUCH_COOP_TOGGLE
	#define VIBRATION_TOGGLE
	#define POINTER_CAR_TOGGLE
	#define DRIVEBY_WEAPONS_TOGGLE
	#define COOP_SKIN_SELECT
	#define FRIENDLY_FIRE_TOGGLE
	#define SHARED_WANTED_TOGGLE
#endif

#ifdef GAMEPAD_MENU
	#define SELECT_CONTROLLER_TYPE  MENUACTION_CFO_SELECT, "FEC_TYP", { new CCFOSelect((int8*)&FrontEndMenuManager.m_PrefsControllerType, "Controller", "Type", controllerTypes, ARRAY_SIZE(controllerTypes), false, ControllerTypeAfterChange) }, 0, 0, MENUALIGN_LEFT,
#else
	#define SELECT_CONTROLLER_TYPE
#endif

const char *filterNames[] = { "FEM_NON", "FEM_SIM", "FEM_NRM", "FEM_MOB" };
const char *off_on[] = { "FEM_OFF", "FEM_ON" };

#ifdef NINTENDO_WII
extern uint32 TimeToStopPadShaking;

const char *pointerBoxSizes[] = { "WII_SML", "WII_MED", "WII_LRG" };
const char *phoneRemoteModes[] = { "FEM_OFF", "WII_RMT", "WII_BTH" };

// The partner's skins.  Index 0 leaves them looking like player 1; the rest are
// the named characters, which Coop.cpp loads into the special-model slots the
// cutscenes use (they are not model slots of their own).
// Text keys, not the words: the menu looks a select's right-hand value up in the
// text tables like every other label on the screen, so a literal here renders as
// "<word> missing" on a card carrying the user's own GXT.  The words are in
// Text.cpp's fallback table with the rest of the port's.
const char *coopSkins[] = { "WII_SK0", "WII_SK1", "WII_SK2", "WII_SK3", "WII_SK4", "WII_SK5", "WII_SK6",
	"WII_SK7", "WII_SK8", "WII_SK9", "WII_SK10", "WII_SK11" };
const int COOP_NUM_SKINS = ARRAY_SIZE(coopSkins);

// A short buzz when vibration is switched on, so it can be felt from the menu.  The
// game is paused behind the menu and a shake is only ever spent down by the game
// clock, so it is also told when to stop (end of CMenuManager::SwitchMenuOnAndOff).
void VibrationAfterChange(int8 before, int8 after) {
	if (after) {
		CPad::GetPad(0)->StartShake(350, 150);
		TimeToStopPadShaking = CTimer::GetTimeInMillisecondsPauseMode() + 500;
	}
}

// The partner's skin is read when they are made, so a change here would not be
// seen until the next time they arrived.  Sent away, they come back with it as
// soon as the menu closes.
void CoopSkinAfterChange(int8 before, int8 after) {
	(void)before;
	(void)after;
	CCoop::Suspend("partner skin");
}
#endif

void RestoreDefGraphics(int8 action) {
	if (action != FEOPTION_ACTION_SELECT)
		return;

	#ifdef PS2_ALPHA_TEST
		gPS2alphaTest = false;
	#endif
	#ifdef MULTISAMPLING
		FrontEndMenuManager.m_nPrefsMSAALevel = FrontEndMenuManager.m_nDisplayMSAALevel = 0;
	#endif
	#ifdef NO_ISLAND_LOADING
	    	if (!FrontEndMenuManager.m_bGameNotLoaded) {
	    		FrontEndMenuManager.m_PrefsIslandLoading = FrontEndMenuManager.ISLAND_LOADING_LOW;
				CStreaming::RemoveUnusedBigBuildings(CGame::currLevel);
				CStreaming::RemoveUnusedBuildings(CGame::currLevel);
				CStreaming::RequestIslands(CGame::currLevel);
		        CStreaming::LoadAllRequestedModels(true);
	    	} else
	    		FrontEndMenuManager.m_PrefsIslandLoading = FrontEndMenuManager.ISLAND_LOADING_LOW;
	#endif
	#ifdef GRAPHICS_MENU_OPTIONS // otherwise Frontend will handle those
		FrontEndMenuManager.m_PrefsFrameLimiter = true;
		FrontEndMenuManager.m_PrefsVsyncDisp = true;
		#ifdef LEGACY_MENU_OPTIONS
			FrontEndMenuManager.m_PrefsVsync = true;
		#endif
		FrontEndMenuManager.m_PrefsUseWideScreen = false;
		FrontEndMenuManager.m_nDisplayVideoMode = FrontEndMenuManager.m_nPrefsVideoMode;
		CMBlur::BlurOn = false;
		FrontEndMenuManager.SaveSettings();
	#endif
}

void RestoreDefDisplay(int8 action) {
	if (action != FEOPTION_ACTION_SELECT)
		return;

	#ifdef CUTSCENE_BORDERS_SWITCH
		FrontEndMenuManager.m_PrefsCutsceneBorders = true;
	#endif
	#ifdef FREE_CAM
		TheCamera.bFreeCam = false;
	#endif
	#ifdef PED_CAR_DENSITY_SLIDERS
		CIniFile::LoadIniFile();
	#endif
	#ifdef GRAPHICS_MENU_OPTIONS // otherwise Frontend will handle those
		FrontEndMenuManager.m_PrefsBrightness = 256;
		FrontEndMenuManager.m_PrefsLOD = 1.2f;
		CRenderer::ms_lodDistScale = 1.2f;
		FrontEndMenuManager.m_PrefsShowSubtitles = false;
		FrontEndMenuManager.m_PrefsShowLegends = true;
		FrontEndMenuManager.m_PrefsRadarMode = 0;
		FrontEndMenuManager.m_PrefsShowHud = true;
		FrontEndMenuManager.SaveSettings();
	#endif
}

#ifdef NO_ISLAND_LOADING
const char *islandLoadingOpts[] = { "FEM_LOW", "FEM_MED", "FEM_HIG" };
void IslandLoadingAfterChange(int8 before, int8 after) {
	if (!FrontEndMenuManager.m_bGameNotLoaded) {
		if (after > FrontEndMenuManager.ISLAND_LOADING_LOW) {
		    FrontEndMenuManager.m_PrefsIslandLoading = before; // calls below needs previous mode :shrug:
		    
		    if (after == FrontEndMenuManager.ISLAND_LOADING_HIGH) {
			    CStreaming::RemoveIslandsNotUsed(LEVEL_BEACH);
			    CStreaming::RemoveIslandsNotUsed(LEVEL_MAINLAND);
			}
		    if (before == FrontEndMenuManager.ISLAND_LOADING_LOW) {
			    FrontEndMenuManager.m_PrefsIslandLoading = after;
			    CStreaming::RequestBigBuildings(CGame::currLevel);
			    
		    } else if (before == FrontEndMenuManager.ISLAND_LOADING_HIGH) {
			    FrontEndMenuManager.m_PrefsIslandLoading = after;
			    CStreaming::RequestIslands(CGame::currLevel);
		    } else
		    	    FrontEndMenuManager.m_PrefsIslandLoading = after;
		    	    
		} else { // low
		    CStreaming::RemoveUnusedBigBuildings(CGame::currLevel);
		    CStreaming::RemoveUnusedBuildings(CGame::currLevel);
		    CStreaming::RequestIslands(CGame::currLevel);
		}

		CStreaming::LoadAllRequestedModels(true);
	}

	FrontEndMenuManager.SetHelperText(0);
}
#endif

#ifdef PED_CAR_DENSITY_SLIDERS
void PedDensityChange(float before, float after) {
	CPopulation::MaxNumberOfPedsInUse = DEFAULT_MAX_NUMBER_OF_PEDS * after;
	CPopulation::MaxNumberOfPedsInUseInterior = DEFAULT_MAX_NUMBER_OF_PEDS_INTERIOR * after;
}

void CarDensityChange(float before, float after) {
	CCarCtrl::MaxNumberOfCarsInUse = DEFAULT_MAX_NUMBER_OF_CARS * after;
}
#endif

#ifndef MULTISAMPLING
void GraphicsGoBack() {
}
#else
void GraphicsGoBack() {
	FrontEndMenuManager.m_nDisplayMSAALevel = FrontEndMenuManager.m_nPrefsMSAALevel;
}

void MultiSamplingButtonPress(int8 action) {
	if (action == FEOPTION_ACTION_SELECT) {
		if (FrontEndMenuManager.m_nDisplayMSAALevel != FrontEndMenuManager.m_nPrefsMSAALevel) {
			FrontEndMenuManager.m_nPrefsMSAALevel = FrontEndMenuManager.m_nDisplayMSAALevel;
			_psSelectScreenVM(FrontEndMenuManager.m_nPrefsVideoMode);
			DMAudio.ChangeMusicMode(MUSICMODE_FRONTEND);
			DMAudio.Service();
			FrontEndMenuManager.SetHelperText(0);
			FrontEndMenuManager.SaveSettings();
		}
	} else if (action == FEOPTION_ACTION_LEFT || action == FEOPTION_ACTION_RIGHT) {
		if (FrontEndMenuManager.m_bGameNotLoaded) {
			FrontEndMenuManager.m_nDisplayMSAALevel += (action == FEOPTION_ACTION_RIGHT ? 1 : -1);

	// The GX device reports no multisampling through the engine query (zero
	// capability); the driver's shift-loop below hangs forever on that, so a
	// zero result clamps the whole row to one level instead.
	int i = 0;
	int maxAA = RwD3D8EngineGetMaxMultiSamplingLevels();
	while (maxAA != 1 && maxAA != 0) {
		i++;
		maxAA >>= 1;
	}

			if (FrontEndMenuManager.m_nDisplayMSAALevel < 0)
				FrontEndMenuManager.m_nDisplayMSAALevel = i;
			else if (FrontEndMenuManager.m_nDisplayMSAALevel > i)
				FrontEndMenuManager.m_nDisplayMSAALevel = 0;
		}
	} else if (action == FEOPTION_ACTION_FOCUSLOSS) {
		if (FrontEndMenuManager.m_nDisplayMSAALevel != FrontEndMenuManager.m_nPrefsMSAALevel) {
			FrontEndMenuManager.m_nDisplayMSAALevel = FrontEndMenuManager.m_nPrefsMSAALevel;
			FrontEndMenuManager.SetHelperText(3);
		}
	}
}

wchar* MultiSamplingDraw(bool *disabled, bool userHovering) {
	static wchar unicodeTemp[64];
	if (userHovering) {
		if (FrontEndMenuManager.m_nDisplayMSAALevel == FrontEndMenuManager.m_nPrefsMSAALevel) {
			if (FrontEndMenuManager.m_nHelperTextMsgId == 1) // Press enter to apply
				FrontEndMenuManager.ResetHelperText();
		} else {
			FrontEndMenuManager.SetHelperText(1);
		}
	} else {
		if (FrontEndMenuManager.m_nDisplayMSAALevel != FrontEndMenuManager.m_nPrefsMSAALevel) {
			FrontEndMenuManager.m_nDisplayMSAALevel = FrontEndMenuManager.m_nPrefsMSAALevel;
		}
	}

	if (!FrontEndMenuManager.m_bGameNotLoaded)
		*disabled = true;

	switch (FrontEndMenuManager.m_nDisplayMSAALevel) {
		case 0:
			return TheText.Get("FEM_OFF");
		default:
			sprintf(gString, "%iX", 1 << (FrontEndMenuManager.m_nDisplayMSAALevel));
			AsciiToUnicode(gString, unicodeTemp);
			return unicodeTemp;
	}
}
#endif

#ifdef IMPROVED_VIDEOMODE
const char* screenModes[] = { "FED_FLS", "FED_WND" };
void ScreenModeAfterChange(int8 before, int8 after)
{
	_psSelectScreenVM(FrontEndMenuManager.m_nPrefsVideoMode); // apply same resolution
	DMAudio.ChangeMusicMode(MUSICMODE_FRONTEND);
	DMAudio.Service();
	FrontEndMenuManager.SetHelperText(0);
}

#endif

#ifdef DETECT_JOYSTICK_MENU
wchar selectedJoystickUnicode[128];
int cachedButtonNum = -1;

wchar* DetectJoystickDraw(bool* disabled, bool userHovering) {

#if defined RW_GL3 && !defined LIBRW_SDL2
	int numButtons;
	int found = -1;
	const char *joyname;
	if (userHovering) {
		for (int i = 0; i <= GLFW_JOYSTICK_LAST; i++) {
			if ((joyname = glfwGetJoystickName(i))) {
				const uint8* buttons = glfwGetJoystickButtons(i, &numButtons);
				for (int j = 0; j < numButtons; j++) {
					if (buttons[j]) {
						found = i;
						break;
					}
				}
				if (found != -1)
					break;
			}
		}

		if (found != -1 && PSGLOBAL(joy1id) != found) {
			if (PSGLOBAL(joy1id) != -1 && PSGLOBAL(joy1id) != found)
				PSGLOBAL(joy2id) = PSGLOBAL(joy1id);
			else
				PSGLOBAL(joy2id) = -1;

			strcpy(gSelectedJoystickName, joyname);
			PSGLOBAL(joy1id) = found;
			cachedButtonNum = numButtons;
		}
	}
	if (PSGLOBAL(joy1id) == -1)
#elif defined XINPUT
	int found = -1;
	XINPUT_STATE xstate;
	memset(&xstate, 0, sizeof(XINPUT_STATE));
	if (userHovering) {
		for (int i = 0; i <= 3; i++) {
			if (XInputGetState(i, &xstate) == ERROR_SUCCESS) {
				if (xstate.Gamepad.bLeftTrigger || xstate.Gamepad.bRightTrigger) {
					found = i;
					break;
				}
				for (int j = XINPUT_GAMEPAD_DPAD_UP; j != XINPUT_GAMEPAD_Y << 1; j = (j << 1)) {
					if (xstate.Gamepad.wButtons & j) {
						found = i;
						break;
					}
				}
				if (found != -1)
					break;
			}
		}
		if (found != -1 && CPad::XInputJoy1 != found) {
			// We should never leave pads -1, so we can process them when they're connected and kinda support hotplug.
			CPad::XInputJoy2 = (CPad::XInputJoy1 == -1 ? (found + 1) % 4 : CPad::XInputJoy1);
			CPad::XInputJoy1 = found;
			cachedButtonNum = 0; // fake too, because xinput bypass CControllerConfig
		}
	}
	sprintf(gSelectedJoystickName, "%d", CPad::XInputJoy1); // fake, on xinput we only store gamepad ids(thanks MS) so this is a temp variable to be used below
	if (CPad::XInputJoy1 == -1)
#endif
		AsciiToUnicode("Not found", selectedJoystickUnicode);
	else
		AsciiToUnicode(gSelectedJoystickName, selectedJoystickUnicode);

	return selectedJoystickUnicode;
}

void DetectJoystickGoBack() {
	if (cachedButtonNum != -1) {
#ifdef LOAD_INI_SETTINGS
		ControlsManager.InitDefaultControlConfigJoyPad(cachedButtonNum);
		SaveINIControllerSettings();
#else
		// Otherwise no way to save gSelectedJoystickName or ms_padButtonsInited anyway :shrug: Why do you even use this config.??
#endif
		cachedButtonNum = -1;
	}
}
#endif

#ifdef GAMEPAD_MENU
const char* controllerTypes[] = { "FEC_DS2", "FEC_DS3", "FEC_DS4", "FEC_360", "FEC_ONE", "FEC_NSW" };
void ControllerTypeAfterChange(int8 before, int8 after)
{
	FrontEndMenuManager.LoadController(after);
}
#endif

#ifdef NINTENDO_WII
// One button wrapper per cheat row.
//
// ButtonPressFunc is void(*)(int8) and carries no identifier, so each row needs its
// own function; the macro keeps them to one line instead of four apiece.  The
// FEOPTION_ACTION_SELECT gate is not decoration: the menu delivers other actions to the
// same handler, and RestoreDefDisplay above guards the same way.
//
// These call the handlers directly rather than synthesising a button sequence.  That
// is the whole point of the menu: the sequences the handlers are matched by are PS2
// chord combinations -- R2 R2 L1 R2 UP DOWN LEFT DOWN RIGHT UP -- and reproducing one
// on a Wii remote would mean guessing at which face button means "R2".
#define WII_CHEAT_BUTTON(name, call) \
	static void name(int8 action) { if(action == FEOPTION_ACTION_SELECT) call; }

WII_CHEAT_BUTTON(WiiCheat_Health,                   (void)HealthCheat())
WII_CHEAT_BUTTON(WiiCheat_Armour,                   (void)ArmourCheat())
WII_CHEAT_BUTTON(WiiCheat_Money,                    (void)MoneyCheat())
WII_CHEAT_BUTTON(WiiCheat_WeaponCheat1,             (void)WeaponCheat1())
WII_CHEAT_BUTTON(WiiCheat_WeaponsForAll,            (void)WeaponsForAllCheat())
WII_CHEAT_BUTTON(WiiCheat_WantedLevelUp,            (void)WantedLevelUpCheat())
WII_CHEAT_BUTTON(WiiCheat_WantedLevelDown,          (void)WantedLevelDownCheat())
WII_CHEAT_BUTTON(WiiCheat_ChangePlayer,             (void)ChangePlayerCheat())
WII_CHEAT_BUTTON(WiiCheat_StrongGrip,               (void)StrongGripCheat())
WII_CHEAT_BUTTON(WiiCheat_Kangaroo,                 (void)KangarooCheat())

WII_CHEAT_BUTTON(WiiCheat_Sunny,                    (void)SunnyWeatherCheat())
WII_CHEAT_BUTTON(WiiCheat_Cloudy,                   (void)CloudyWeatherCheat())
WII_CHEAT_BUTTON(WiiCheat_Rainy,                    (void)RainyWeatherCheat())
WII_CHEAT_BUTTON(WiiCheat_Foggy,                    (void)FoggyWeatherCheat())
WII_CHEAT_BUTTON(WiiCheat_FastWeather,              (void)FastWeatherCheat())
WII_CHEAT_BUTTON(WiiCheat_Mayhem,                   (void)MayhemCheat())
WII_CHEAT_BUTTON(WiiCheat_EverybodyAttacksPlayer,   (void)EverybodyAttacksPlayerCheat())
WII_CHEAT_BUTTON(WiiCheat_BlowUpCars,               (void)BlowUpCarsCheat())
WII_CHEAT_BUTTON(WiiCheat_WallClimbing,             (void)WallClimbingCheat())
WII_CHEAT_BUTTON(WiiCheat_NoSeaBed,                 (void)NoSeaBedCheat())

WII_CHEAT_BUTTON(WiiCheat_AllCarsHeli,              (void)AllCarsHeliCheat())
// The ten vehicles the original keyboard cheats spawn.  VehicleCheat takes a
// model rather than being a toggle, so each gets a button of its own; the row
// carries the cheat word the desktop build types for it (Pad.cpp's matcher),
// so the menu and the keyboard do the same thing.
WII_CHEAT_BUTTON(WiiCheat_Rhino,                    (void)VehicleCheat(MI_RHINO))
WII_CHEAT_BUTTON(WiiCheat_Bloodra,                  (void)VehicleCheat(MI_BLOODRA))
WII_CHEAT_BUTTON(WiiCheat_Romero,                   (void)VehicleCheat(MI_ROMERO))
WII_CHEAT_BUTTON(WiiCheat_Lovefist,                 (void)VehicleCheat(MI_LOVEFIST))
WII_CHEAT_BUTTON(WiiCheat_Trash,                    (void)VehicleCheat(MI_TRASH))
WII_CHEAT_BUTTON(WiiCheat_Bloodrb,                  (void)VehicleCheat(MI_BLOODRB))
WII_CHEAT_BUTTON(WiiCheat_Sabretur,                 (void)VehicleCheat(MI_SABRETUR))
WII_CHEAT_BUTTON(WiiCheat_Caddy,                    (void)VehicleCheat(MI_CADDY))
WII_CHEAT_BUTTON(WiiCheat_Hotrina,                  (void)VehicleCheat(MI_HOTRINA))
WII_CHEAT_BUTTON(WiiCheat_Hotrinb,                  (void)VehicleCheat(MI_HOTRINB))
WII_CHEAT_BUTTON(WiiCheat_ChittyChittyBangBang,     (void)ChittyChittyBangBangCheat())

WII_CHEAT_BUTTON(WiiCheat_FastTime,                 (void)FastTimeCheat())
WII_CHEAT_BUTTON(WiiCheat_SlowTime,                 (void)SlowTimeCheat())
WII_CHEAT_BUTTON(WiiCheat_OnlyRenderWheels,         (void)OnlyRenderWheelsCheat())
WII_CHEAT_BUTTON(WiiCheat_RenderWaterLayers,        (void)RenderWaterLayersCheat())
WII_CHEAT_BUTTON(WiiCheat_SwitchDebugDisplay,       (void)CPed::SwitchDebugDisplay())

#undef WII_CHEAT_BUTTON
#endif

CMenuScreenCustom aScreens[] = {
	// MENUPAGE_STATS = 0
	{ "FEH_STA", MENUPAGE_NONE, nil, nil,
		MENUACTION_GOBACK, "FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 190, 320, MENUALIGN_RIGHT,
	},

	// MENUPAGE_NEW_GAME = 1
	{ "FEP_STG", MENUPAGE_NONE, nil, nil,
		MENUACTION_CHANGEMENU, "FES_NGA", {nil, SAVESLOT_NONE, MENUPAGE_NEW_GAME_RELOAD}, 320, 155, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU, "FES_LOA",  {nil, SAVESLOT_NONE, MENUPAGE_CHOOSE_LOAD_SLOT}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU, "FES_DEL", {nil, SAVESLOT_NONE, MENUPAGE_CHOOSE_DELETE_SLOT}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_GOBACK, "FEDS_TB", {nil, SAVESLOT_NONE, 0}, 0, 0, MENUALIGN_CENTER,
	},

	// MENUPAGE_BRIEFS = 2
	{ "FEH_BRI", MENUPAGE_NONE, nil, nil,
		MENUACTION_GOBACK, "FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 190, 320, MENUALIGN_RIGHT,
	},

	// MENUPAGE_SOUND_SETTINGS = 3
	{ "FEH_AUD", MENUPAGE_OPTIONS, nil, nil,
		MENUACTION_MUSICVOLUME,		"FEA_MUS", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 40, 76, MENUALIGN_LEFT,
		MENUACTION_SFXVOLUME,		"FEA_SFX", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_MP3VOLUMEBOOST,	"FEA_MPB", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 0, 0, MENUALIGN_LEFT,
#ifdef EXTERNAL_3D_SOUND
		MENUACTION_AUDIOHW,			"FEA_3DH", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SPEAKERCONF,		"FEA_SPK", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 0, 0, MENUALIGN_LEFT,
#endif
		MENUACTION_DYNAMICACOUSTIC,	"FET_DAM", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_RADIO,			"FEA_RSS", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 0, 0, MENUALIGN_LEFT,
#ifdef NINTENDO_WII
		MENUACTION_CFO_SELECT,		"WII_RMK", { new CCFOSelect((int8*)&WiiRemoteSpeakerEnabled, "Wii", "RemoteSpeaker", off_on, 2, false) }, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CFO_SELECT,		"WII_PHN", { new CCFOSelect((int8*)&WiiPhoneRemoteMode, "Wii", "PhoneCalls", phoneRemoteModes, 3, false) }, 0, 0, MENUALIGN_LEFT,
#endif
#ifdef EXTERNAL_3D_SOUND
		MENUACTION_RESTOREDEF,		"FET_DEF", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 320, 367, MENUALIGN_CENTER,
#else
		MENUACTION_RESTOREDEF,		"FET_DEF", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 320, 327, MENUALIGN_CENTER,
#endif
		MENUACTION_GOBACK,			"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, MENUALIGN_CENTER,
	},

	// MENUPAGE_DISPLAY_SETTINGS = 4
#ifndef GRAPHICS_MENU_OPTIONS
	{ "FEH_DIS", MENUPAGE_OPTIONS, new CCustomScreenLayout({40, 78, 25, true}), nil,
		MENUACTION_BRIGHTNESS,	"FED_BRI", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_DRAWDIST,	"FEM_LOD", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		DENSITY_SLIDERS
#ifdef LEGACY_MENU_OPTIONS
		MENUACTION_FRAMESYNC,	"FEM_VSC", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
#endif
		MENUACTION_FRAMELIMIT,	"FEM_FRM", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
#if defined LEGACY_MENU_OPTIONS && !defined EXTENDED_COLOURFILTER
		MENUACTION_TRAILS,		"FED_TRA", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
#endif
		MENUACTION_SUBTITLES,	"FED_SUB", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_WIDESCREEN,	"FED_WIS", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_LEGENDS,		"MAP_LEG", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_RADARMODE,	"FED_RDR", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_HUD,			"FED_HUD", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SCREENRES,	"FED_RES", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_LEFT,
		VIDEOMODE_SELECTOR
		MULTISAMPLING_SELECTOR
		ISLAND_LOADING_SELECTOR
		DUALPASS_SELECTOR
		CUTSCENE_BORDERS_TOGGLE
		FREE_CAM_TOGGLE
		POSTFX_SELECTORS
		// re3.cpp inserts here pipeline selectors if neo/neo.txd exists and EXTENDED_PIPELINES defined
		MENUACTION_RESTOREDEF,	"FET_DEF", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 320, 0, MENUALIGN_CENTER,
		MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 320, 0, MENUALIGN_CENTER,
	},
#else
	{ "FEH_DIS", MENUPAGE_OPTIONS, new CCustomScreenLayout({40, 78, 25, true}), nil,
		MENUACTION_BRIGHTNESS,	"FED_BRI", { nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS }, 0, 0, MENUALIGN_LEFT,
		MENUACTION_DRAWDIST,	"FEM_LOD", { nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS }, 0, 0, MENUALIGN_LEFT,
		DENSITY_SLIDERS
		CUTSCENE_BORDERS_TOGGLE
		FREE_CAM_TOGGLE
		MENUACTION_LEGENDS,		"MAP_LEG", { nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS }, 0, 0, MENUALIGN_LEFT,
		MENUACTION_RADARMODE,	"FED_RDR", { nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS }, 0, 0, MENUALIGN_LEFT,
		MENUACTION_HUD,			"FED_HUD", { nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS }, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SUBTITLES,	"FED_SUB", { nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS }, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CFO_DYNAMIC,	"FET_DEF", { new CCFODynamic(nil, nil, nil, nil, RestoreDefDisplay) }, 320, 0, MENUALIGN_CENTER,
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE}, 320, 0, MENUALIGN_CENTER,
	},
#endif

	// MENUPAGE_LANGUAGE_SETTINGS = 5
	{ "FEH_LAN", MENUPAGE_OPTIONS, nil, nil,
		MENUACTION_LANG_ENG,	"FEL_ENG", {nil, SAVESLOT_NONE, MENUPAGE_LANGUAGE_SETTINGS}, 320, 132, MENUALIGN_CENTER,
		MENUACTION_LANG_FRE,	"FEL_FRE", {nil, SAVESLOT_NONE, MENUPAGE_LANGUAGE_SETTINGS}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_LANG_GER,	"FEL_GER", {nil, SAVESLOT_NONE, MENUPAGE_LANGUAGE_SETTINGS}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_LANG_ITA,	"FEL_ITA", {nil, SAVESLOT_NONE, MENUPAGE_LANGUAGE_SETTINGS}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_LANG_SPA,    "FEL_SPA", {nil, SAVESLOT_NONE, MENUPAGE_LANGUAGE_SETTINGS}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, MENUALIGN_CENTER,
	},

	// MENUPAGE_MAP = 6
	{ "FEH_MAP", MENUPAGE_NONE, nil, nil,
		 MENUACTION_GOBACK,	"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 70, 380, MENUALIGN_CENTER,
	},

	// MENUPAGE_NEW_GAME_RELOAD = 7
	{ "FES_NGA", MENUPAGE_NEW_GAME, nil, nil,
		MENUACTION_LABEL,		"FESZ_QR",	{nil, SAVESLOT_NONE,	0}, 0, 0, 0,
		MENUACTION_NO,			"FEM_NO",	{nil, SAVESLOT_NONE,	MENUPAGE_NEW_GAME}, 320, 200, MENUALIGN_CENTER,
		MENUACTION_NEWGAME,		"FEM_YES",	{nil, SAVESLOT_NONE,	MENUPAGE_NEW_GAME_RELOAD}, 320, 225, MENUALIGN_CENTER,
	},

	// MENUPAGE_CHOOSE_LOAD_SLOT = 8
	{ "FET_LG", MENUPAGE_NEW_GAME, nil, nil,
		MENUACTION_CHECKSAVE,	"FEM_SL1", {nil, SAVESLOT_1,		0}, 40, 90, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL2", {nil, SAVESLOT_2,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL3", {nil, SAVESLOT_3,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL4", {nil, SAVESLOT_4,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL5", {nil, SAVESLOT_5,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL6", {nil, SAVESLOT_6,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL7", {nil, SAVESLOT_7,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL8", {nil, SAVESLOT_8,		0}, 0, 0, MENUALIGN_LEFT,
		// The post-mission quicksave, shown as its own row so the player can see
		// when it last ran and load it directly.
		MENUACTION_CHECKSAVE,	"WII_QSV", {nil, SAVESLOT_QUICKSAVE,	0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE,	0}, 320, 345, MENUALIGN_CENTER,
	},

	// MENUPAGE_CHOOSE_DELETE_SLOT = 9
	{ "FES_DEL", MENUPAGE_NEW_GAME, nil, nil,
		MENUACTION_CHECKSAVE,	"FEM_SL1",	{nil, SAVESLOT_1,		0}, 40, 90, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL2",	{nil, SAVESLOT_2,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL3",	{nil, SAVESLOT_3,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL4",	{nil, SAVESLOT_4,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL5",	{nil, SAVESLOT_5,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL6",	{nil, SAVESLOT_6,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL7",	{nil, SAVESLOT_7,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_CHECKSAVE,	"FEM_SL8",	{nil, SAVESLOT_8,		0}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_GOBACK,		"FEDS_TB",	{nil, SAVESLOT_NONE,	0}, 320, 345, MENUALIGN_CENTER,
	},

	// MENUPAGE_LOAD_SLOT_CONFIRM = 10
	{ "FET_LG", MENUPAGE_CHOOSE_LOAD_SLOT, nil, nil,
		 MENUACTION_LABEL,		"FESZ_QL",	{nil, SAVESLOT_NONE,	0}, 0, 0, 0,
		 MENUACTION_NO,			"FEM_NO",	{nil, SAVESLOT_NONE,	MENUPAGE_CHOOSE_LOAD_SLOT}, 320, 200, MENUALIGN_CENTER,
		 MENUACTION_YES,		"FEM_YES",	{nil, SAVESLOT_NONE,	MENUPAGE_LOADING_IN_PROGRESS}, 320, 225, MENUALIGN_CENTER,
	},

	// MENUPAGE_DELETE_SLOT_CONFIRM = 11
	{ "FES_DEL", MENUPAGE_CHOOSE_DELETE_SLOT, nil, nil,
		 MENUACTION_LABEL,		"FESZ_QD",	{nil, SAVESLOT_NONE,  MENUPAGE_NONE}, 0, 0, 0,
		 MENUACTION_NO,			"FEM_NO",	{nil, SAVESLOT_NONE,  MENUPAGE_CHOOSE_DELETE_SLOT}, 320, 200, MENUALIGN_CENTER,
		 MENUACTION_YES,		"FEM_YES",	{nil, SAVESLOT_NONE,	MENUPAGE_DELETING_IN_PROGRESS}, 320, 225, MENUALIGN_CENTER,
	},

	// MENUPAGE_LOADING_IN_PROGRESS = 12
	{ "FET_LG", MENUPAGE_CHOOSE_LOAD_SLOT, nil, nil,
	},

	// MENUPAGE_DELETING_IN_PROGRESS = 13
	{ "FES_DEL", MENUPAGE_CHOOSE_DELETE_SLOT, nil, nil,
	},

	// MENUPAGE_DELETE_SUCCESSFUL = 14
	{ "FES_DEL", MENUPAGE_NEW_GAME, nil, nil,
		 MENUACTION_LABEL,		"FES_DSC",	{nil, SAVESLOT_NONE,	0}, 0, 0, 0,
		 MENUACTION_CHANGEMENU,	"FEM_OK",	{nil, SAVESLOT_NONE,	MENUPAGE_NEW_GAME}, 320, 225, MENUALIGN_CENTER,
	},

	// MENUPAGE_CHOOSE_SAVE_SLOT = 15
	{ "FET_SG", MENUPAGE_DISABLED, nil, nil,
		MENUACTION_SAVEGAME,			"FEM_SL1", {nil, SAVESLOT_1,		MENUPAGE_SAVE_OVERWRITE_CONFIRM}, 40, 90, MENUALIGN_LEFT,
		MENUACTION_SAVEGAME,			"FEM_SL2", {nil, SAVESLOT_2,		MENUPAGE_SAVE_OVERWRITE_CONFIRM}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SAVEGAME,			"FEM_SL3", {nil, SAVESLOT_3,		MENUPAGE_SAVE_OVERWRITE_CONFIRM}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SAVEGAME,			"FEM_SL4", {nil, SAVESLOT_4,		MENUPAGE_SAVE_OVERWRITE_CONFIRM}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SAVEGAME,			"FEM_SL5", {nil, SAVESLOT_5,		MENUPAGE_SAVE_OVERWRITE_CONFIRM}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SAVEGAME,			"FEM_SL6", {nil, SAVESLOT_6,		MENUPAGE_SAVE_OVERWRITE_CONFIRM}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SAVEGAME,			"FEM_SL7", {nil, SAVESLOT_7,		MENUPAGE_SAVE_OVERWRITE_CONFIRM}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_SAVEGAME,			"FEM_SL8", {nil, SAVESLOT_8,		MENUPAGE_SAVE_OVERWRITE_CONFIRM}, 0, 0, MENUALIGN_LEFT,
		MENUACTION_RESUME_FROM_SAVEZONE,"FESZ_CA", {nil, SAVESLOT_NONE,	0}, 320, 345, MENUALIGN_CENTER,
	},

	// MENUPAGE_SAVE_OVERWRITE_CONFIRM = 16
	{ "FET_SG", MENUPAGE_CHOOSE_SAVE_SLOT, nil, nil,
		MENUACTION_LABEL,		"FESZ_QZ", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
		MENUACTION_NO,			"FEM_NO", {nil, SAVESLOT_NONE, MENUPAGE_CHOOSE_SAVE_SLOT}, 320, 200, MENUALIGN_CENTER,
		MENUACTION_YES,			"FEM_YES",  {nil, SAVESLOT_NONE, MENUPAGE_SAVING_IN_PROGRESS}, 320, 225, MENUALIGN_CENTER,
	},

	// MENUPAGE_SAVING_IN_PROGRESS = 17
	{ "FET_SG", MENUPAGE_CHOOSE_SAVE_SLOT, nil, nil,
	},

	// MENUPAGE_SAVE_SUCCESSFUL = 18
	{ "FET_SG", MENUPAGE_CHOOSE_SAVE_SLOT, nil, nil,
		MENUACTION_LABEL,					"FES_SSC",	{nil, SAVESLOT_LABEL,	MENUPAGE_NONE}, 0, 0, 0,
		MENUACTION_RESUME_FROM_SAVEZONE,	"FEM_OK",	{nil, SAVESLOT_NONE,	MENUPAGE_CHOOSE_SAVE_SLOT}, 320, 225, MENUALIGN_CENTER,
	},

	// MENUPAGE_SAVE_CUSTOM_WARNING = 19
	{ "FET_SG", MENUPAGE_NONE, nil, nil,
		MENUACTION_LABEL,		"",			{nil, SAVESLOT_NONE, 0}, 0, 0, 0,
		MENUACTION_CHANGEMENU,	"FEM_OK",	{nil, SAVESLOT_NONE, MENUPAGE_CHOOSE_SAVE_SLOT}, 320, 225, MENUALIGN_CENTER,
	},

	// MENUPAGE_SAVE_CHEAT_WARNING = 20
	{ "FET_SG", MENUPAGE_NEW_GAME, nil, nil,
		MENUACTION_LABEL,		"FES_CHE",	{nil, SAVESLOT_NONE,	MENUPAGE_NONE}, 0, 0, 0,
		MENUACTION_CHANGEMENU,	"FEM_OK",	{nil, SAVESLOT_NONE,	MENUPAGE_CHOOSE_SAVE_SLOT}, 320, 225, MENUALIGN_CENTER,
	},

	// MENUPAGE_SKIN_SELECT = 21
	{ "FET_PS", MENUPAGE_OPTIONS, nil, nil,
		 MENUACTION_GOBACK,		"FEDS_TB",	{nil, SAVESLOT_NONE, MENUPAGE_OPTIONS}, 0, 0, 0,
	},

	// MENUPAGE_SAVE_UNUSED = 22
	{ "FET_SG", MENUPAGE_NEW_GAME, nil, nil,
		 MENUACTION_LABEL,		"FED_LWR",	{nil, SAVESLOT_NONE,	0}, 0, 0, 0,
		 MENUACTION_CHANGEMENU,	"FEC_OKK",	{nil, SAVESLOT_NONE,	MENUPAGE_CHOOSE_SAVE_SLOT}, 0, 0, 0,
	},

	// MENUPAGE_SAVE_FAILED = 23
	{ "FET_SG", MENUPAGE_CHOOSE_SAVE_SLOT, nil, nil,
		 MENUACTION_LABEL,		"FEC_SVU",	{nil, SAVESLOT_NONE,	0}, 0, 0, 0,
		 MENUACTION_CHANGEMENU,	"FEC_OKK",	{nil, SAVESLOT_NONE,	MENUPAGE_CHOOSE_SAVE_SLOT}, 0, 0, 0,
	},

	// MENUPAGE_SAVE_FAILED_2 = 24
	{ "FET_LG", MENUPAGE_CHOOSE_SAVE_SLOT, nil, nil,
		 MENUACTION_LABEL,		"FEC_SVU",	{nil, SAVESLOT_NONE,	0}, 0, 0, 0,
	},

	// MENUPAGE_LOAD_FAILED = 25
	{ "FET_LG", MENUPAGE_NEW_GAME, nil, nil,
		 MENUACTION_LABEL,		"FEC_LUN",	{nil, SAVESLOT_NONE,  0}, 0, 0, 0,
		 MENUACTION_GOBACK,		"FEDS_TB",	{nil, SAVESLOT_NONE,  MENUPAGE_NEW_GAME}, 0, 0, 0,
	},

	// MENUPAGE_CONTROLLER_PC = 26
	{ "FET_CTL", MENUPAGE_OPTIONS, new CCustomScreenLayout({0, 0, MENU_DEFAULT_LINE_HEIGHT, false, false, 150}), nil,
#ifdef PC_PLAYER_CONTROLS
		// The control-method toggle (Standard free aim / Classic lock-on) is live on
		// the Wii.  The keyboard-binding page is not: it is rows of GETKEY binds
		// that no keyboard can ever answer, and entering it leaves the menu
		// waiting on the nonexistent device.  Mouse/IR settings and the
		// restore-defaults lever are real.
#ifndef NINTENDO_WII
		MENUACTION_CTRLMETHOD,	"FET_STI", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC}, 320, 150, MENUALIGN_CENTER,
		MENUACTION_KEYBOARDCTRLS,"FEC_RED", {nil, SAVESLOT_NONE, MENUPAGE_KEYBOARD_CONTROLS}, 0, 0, MENUALIGN_CENTER,
#else
		MENUACTION_CTRLMETHOD,	"FET_STI", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC}, 320, 135, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU,	"FEC_MOU", {nil, SAVESLOT_NONE, MENUPAGE_MOUSE_CONTROLS}, 0, 0, MENUALIGN_CENTER,
#endif
#else
		MENUACTION_KEYBOARDCTRLS,"FEC_RED", {nil, SAVESLOT_NONE, MENUPAGE_KEYBOARD_CONTROLS}, 320, 150, MENUALIGN_CENTER,
#endif
#ifdef GAMEPAD_MENU
		MENUACTION_CHANGEMENU,	"FET_AGS", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_SETTINGS}, 0, 0, MENUALIGN_CENTER,
#endif
#ifdef DETECT_JOYSTICK_MENU
		MENUACTION_CHANGEMENU,	"FEC_JOD", {nil, SAVESLOT_NONE, MENUPAGE_DETECT_JOYSTICK}, 0, 0, MENUALIGN_CENTER,
#endif
#ifndef NINTENDO_WII
		MENUACTION_CHANGEMENU,	"FEC_MOU", {nil, SAVESLOT_NONE, MENUPAGE_MOUSE_CONTROLS}, 0, 0, MENUALIGN_CENTER,
#endif
		MENUACTION_RESTOREDEF,	"FET_DEF", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC}, 320, 0, MENUALIGN_CENTER,
		MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE, 0}, 320, 0, MENUALIGN_CENTER,
   },

	// MENUPAGE_OPTIONS = 27
	{ "FET_OPT", MENUPAGE_NONE, nil, nil,
#ifdef GTA_HANDHELD
		 MENUACTION_CHANGEMENU,		"FEO_CON", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_SETTINGS}, 320, 132, MENUALIGN_CENTER,
#else
		 MENUACTION_CHANGEMENU,		"FEO_CON", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC}, 320, 132, MENUALIGN_CENTER,
#endif
		 MENUACTION_LOADRADIO,		"FEO_AUD", {nil, SAVESLOT_NONE, MENUPAGE_SOUND_SETTINGS}, 0, 0, MENUALIGN_CENTER,
		 MENUACTION_CHANGEMENU,		"FEO_DIS", {nil, SAVESLOT_NONE, MENUPAGE_DISPLAY_SETTINGS}, 0, 0, MENUALIGN_CENTER,
#ifdef GRAPHICS_MENU_OPTIONS
		 MENUACTION_CHANGEMENU,		"FET_GFX", {nil, SAVESLOT_NONE, MENUPAGE_GRAPHICS_SETTINGS}, 0, 0, MENUALIGN_CENTER,
#endif
		 MENUACTION_CHANGEMENU,		"FEO_LAN", {nil, SAVESLOT_NONE, MENUPAGE_LANGUAGE_SETTINGS}, 0, 0, MENUALIGN_CENTER,
		 MENUACTION_PLAYERSETUP,	"FET_PS", {nil, SAVESLOT_NONE, MENUPAGE_SKIN_SELECT}, 0, 0, MENUALIGN_CENTER,
		 MENUACTION_GOBACK,			"FEDS_TB", {nil, SAVESLOT_NONE, 0}, 0, 0, MENUALIGN_CENTER,
   },

	// MENUPAGE_EXIT = 28
	{ "FET_QG", MENUPAGE_NONE, nil, nil,
		MENUACTION_LABEL,		"FEQ_SRE",	{nil, SAVESLOT_NONE, 0}, 0, 0, 0,
		MENUACTION_DONTCANCEL,	"FEM_NO",	{nil, SAVESLOT_NONE, MENUPAGE_NONE}, 320, 200, MENUALIGN_CENTER,
		MENUACTION_CANCELGAME,	"FEM_YES",	{nil, SAVESLOT_NONE, MENUPAGE_NONE}, 320, 225, MENUALIGN_CENTER,
   },

	// MENUPAGE_START_MENU = 29
	{ "FEM_MM", MENUPAGE_DISABLED, nil, nil,
		  MENUACTION_CHANGEMENU,	"FEP_STG",	{nil, SAVESLOT_NONE,	MENUPAGE_NEW_GAME}, 320, 170, MENUALIGN_CENTER,
		  MENUACTION_CHANGEMENU,	"FEP_OPT",	{nil, SAVESLOT_NONE,	MENUPAGE_OPTIONS}, 0, 0, MENUALIGN_CENTER,
		  MENUACTION_CHANGEMENU,	"FEP_QUI",	{nil, SAVESLOT_NONE,	MENUPAGE_EXIT}, 0, 0, MENUALIGN_CENTER,
   },

	// MENUPAGE_KEYBOARD_CONTROLS = 30
	{ "FET_STI", MENUPAGE_CONTROLLER_PC, nil, nil,
   },

	// MENUPAGE_MOUSE_CONTROLS = 31
	{ "FEC_MOU", MENUPAGE_CONTROLLER_PC, nil, nil,
		// The port's toggles used to be on this page, which is why it started
		// higher to keep Back on screen.  They are on the Enhancements page now, so
		// this is the plain mouse/IR page again and starts where the non-Wii one
		// does.
		MENUACTION_MOUSESENS,	"FEC_MSH",	{nil, SAVESLOT_NONE, MENUPAGE_MOUSE_CONTROLS}, 40, 170, MENUALIGN_LEFT,
		MENUACTION_INVVERT,		"FEC_IVV",	{nil, SAVESLOT_NONE, MENUPAGE_MOUSE_CONTROLS}, 0, 0, MENUALIGN_LEFT,
#ifndef GAMEPAD_MENU
	   INVERT_PAD_SELECTOR
#endif
#ifndef NINTENDO_WII
		// Not on the Wii: there the pointer is the camera, never the steering wheel
		// (WiiPadApplyControlDefaults keeps mouse steering off).
		MENUACTION_MOUSESTEER,	"FET_MST",	{nil, SAVESLOT_NONE, MENUPAGE_MOUSE_CONTROLS}, 0, 0, MENUALIGN_LEFT,
#endif
		// The port's own toggles used to sit here.  They are on the Enhancements page
		// now, which is where a player looking for them would go; this page is the
		// mouse/IR settings it says it is.
		MENUACTION_GOBACK,		"FEDS_TB",	{nil, SAVESLOT_NONE, 0}, 320, 0, MENUALIGN_CENTER,
		//MENUACTION_GOBACK,		"FEDS_TB",	{nil, SAVESLOT_NONE, 0}, 320, 260, MENUALIGN_CENTER, // original y
   },

	// MENUPAGE_PAUSE_MENU = 32
	{ "FET_PAU", MENUPAGE_DISABLED, nil, nil,
		MENUACTION_RESUME,		"FEP_RES",	{nil, SAVESLOT_NONE, 0}, 320, 120, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU,	"FEH_SGA",	{nil, SAVESLOT_NONE, MENUPAGE_NEW_GAME}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU,	"FEH_MAP",	{nil, SAVESLOT_NONE, MENUPAGE_MAP}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU,	"FEP_STA",	{nil, SAVESLOT_NONE, MENUPAGE_STATS}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU,	"FEH_BRI",	{nil, SAVESLOT_NONE, MENUPAGE_BRIEFS}, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU,	"FET_OPT",	{nil, SAVESLOT_NONE, MENUPAGE_OPTIONS}, 0, 0, MENUALIGN_CENTER,
#ifdef NINTENDO_WII
		// Everything this port added lives behind this one row, so the pause menu stays
		// the set the game shipped with and the cheats, co-op and the rest are not
		// scattered over pages that were never meant to hold them.  Reachable from the
		// pause menu because that is the only menu the player can open mid-game on this
		// port: the cheats used to be a PS2 button sequence, which cannot be typed on a
		// Wii remote.
		MENUACTION_CHANGEMENU,	"WII_ENH",	{nil, SAVESLOT_NONE, MENUPAGE_ENHANCEMENTS}, 0, 0, MENUALIGN_CENTER,
#endif
		MENUACTION_CHANGEMENU,	"FEP_QUI",	{nil, SAVESLOT_NONE, MENUPAGE_EXIT}, 0, 0, MENUALIGN_CENTER,
   },

	// MENUPAGE_NONE = 33
	{ "", 0, nil, nil, },

#ifdef GAMEPAD_MENU
#ifdef GTA_HANDHELD
	{ "FET_AGS", MENUPAGE_OPTIONS, new CCustomScreenLayout({40, 78, 25, true, true}), nil,
#else
	{ "FET_AGS", MENUPAGE_CONTROLLER_PC, new CCustomScreenLayout({40, 78, 25, true, true}), nil,
#endif
		MENUACTION_CTRLCONFIG,		"FEC_CCF", { nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_SETTINGS }, 40, 76, MENUALIGN_LEFT,
		MENUACTION_CTRLDISPLAY,		"FEC_CDP", { nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_SETTINGS }, 0, 0, MENUALIGN_LEFT,
		INVERT_PAD_SELECTOR
		MENUACTION_CTRLVIBRATION,	"FEC_VIB", { nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_SETTINGS }, 0, 0, MENUALIGN_LEFT,
		SELECT_CONTROLLER_TYPE
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, MENUALIGN_LEFT,
	},
#endif
#ifdef LEGACY_MENU_OPTIONS
	// MENUPAGE_DEBUG_MENU = 18
	{ "FED_DBG", MENUPAGE_NONE, nil, nil,
		MENUACTION_RELOADIDE,	"FED_RID", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
		MENUACTION_SETDBGFLAG,	"FED_DFL", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
		MENUACTION_SWITCHBIGWHITEDEBUGLIGHT,	"FED_DLS", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
		MENUACTION_COLLISIONPOLYS,	"FED_SCP", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
		MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
   },

	// MENUPAGE_CONTROLLER_PC_OLD1 = 36
	{ "FET_CTL", MENUPAGE_CONTROLLER_PC, nil, nil,
		MENUACTION_GETKEY,	"FEC_PLB", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GETKEY,	"FEC_CWL", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GETKEY,	"FEC_CWR", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GETKEY,	"FEC_LKT", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GETKEY,	"FEC_PJP", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GETKEY,	"FEC_PSP", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GETKEY,	"FEC_TLF", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GETKEY,	"FEC_TRG", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GETKEY,	"FEC_CCM", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD1}, 0, 0, 0,
		MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
   },

	// MENUPAGE_CONTROLLER_PC_OLD2 = 37
	{ "FET_CTL", MENUPAGE_CONTROLLER_PC, nil, nil,

	},

	// MENUPAGE_CONTROLLER_PC_OLD3 = 38
	{ "FET_CTL", MENUPAGE_CONTROLLER_PC, nil, nil,
		 MENUACTION_GETKEY,	"FEC_LUP", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD3}, 0, 0, 0,
		 MENUACTION_GETKEY,	"FEC_LDN", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD3}, 0, 0, 0,
		 MENUACTION_GETKEY,	"FEC_SMS", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD3}, 0, 0, 0,
		 MENUACTION_SHOWHEADBOB,	"FEC_GSL", {nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_PC_OLD3}, 0, 0, 0,
		 MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
   },

	// MENUPAGE_CONTROLLER_PC_OLD4 = 39
	{ "FET_CTL", MENUPAGE_CONTROLLER_PC, nil, nil,

	},

	// MENUPAGE_CONTROLLER_DEBUG = 40
	{ "FEC_DBG", MENUPAGE_CONTROLLER_PC, nil, nil,
		 MENUACTION_GETKEY,	"FEC_TGD",	{nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_DEBUG}, 0, 0, 0,
		 MENUACTION_GETKEY,	"FEC_TDO",	{nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_DEBUG}, 0, 0, 0,
		 MENUACTION_GETKEY,	"FEC_TSS",	{nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_DEBUG}, 0, 0, 0,
		 MENUACTION_GETKEY,	"FEC_SMS",	{nil, SAVESLOT_NONE, MENUPAGE_CONTROLLER_DEBUG}, 0, 0, 0,
		 MENUACTION_GOBACK,	"FEDS_TB",	{nil, SAVESLOT_NONE, MENUPAGE_NONE}, 0, 0, 0,
   },
#endif

#ifdef GRAPHICS_MENU_OPTIONS
	// MENUPAGE_GRAPHICS_SETTINGS
	{ "FET_GFX", MENUPAGE_OPTIONS, new CCustomScreenLayout({40, 78, 25, true, true}), GraphicsGoBack,

#ifndef GTA_HANDHELD
	// The Resolution row and the video-mode selector are PC plumbing: on the
	// Wii the GX mode is one fixed 480i framebuffer, so both rows read
	// hardware that does not exist.  Multisampling is the same story -- the GX
	// engine reports no MSAA levels at all.
#ifndef NINTENDO_WII
		MENUACTION_SCREENRES,	"FED_RES", { nil, SAVESLOT_NONE, MENUPAGE_GRAPHICS_SETTINGS }, 0, 0, MENUALIGN_LEFT,
#endif
#endif
		MENUACTION_WIDESCREEN,	"FED_WIS", { nil, SAVESLOT_NONE, MENUPAGE_GRAPHICS_SETTINGS }, 0, 0, MENUALIGN_LEFT,
#ifndef NINTENDO_WII
		VIDEOMODE_SELECTOR
#endif
#ifdef LEGACY_MENU_OPTIONS
		MENUACTION_FRAMESYNC,	"FEM_VSC", {nil, SAVESLOT_NONE, MENUPAGE_GRAPHICS_SETTINGS}, 0, 0, MENUALIGN_LEFT,
#endif
		MENUACTION_FRAMELIMIT,	"FEM_FRM", { nil, SAVESLOT_NONE, MENUPAGE_GRAPHICS_SETTINGS }, 0, 0, MENUALIGN_LEFT,
#ifndef NINTENDO_WII
		MULTISAMPLING_SELECTOR
#endif
		ISLAND_LOADING_SELECTOR
		DUALPASS_SELECTOR
#ifdef EXTENDED_COLOURFILTER
		POSTFX_SELECTORS
#elif defined LEGACY_MENU_OPTIONS
		MENUACTION_TRAILS,		"FED_TRA", { nil, SAVESLOT_NONE, MENUPAGE_GRAPHICS_SETTINGS }, 0, 0, MENUALIGN_LEFT,
#endif
		// re3.cpp inserts here pipeline selectors if neo/neo.txd exists and EXTENDED_PIPELINES defined
		MENUACTION_CFO_DYNAMIC,	"FET_DEF", { new CCFODynamic(nil, nil, nil, nil, RestoreDefGraphics) }, 320, 0, MENUALIGN_CENTER,
		MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 320, 0, MENUALIGN_CENTER,
	},
#endif

#ifdef DETECT_JOYSTICK_MENU
	// MENUPAGE_DETECT_JOYSTICK
	{ "FEC_JOD", MENUPAGE_CONTROLLER_PC, new CCustomScreenLayout({0, 0, 0, false, false, 30}), DetectJoystickGoBack,
		MENUACTION_LABEL,	"FEC_JPR", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, 0,
		MENUACTION_CFO_DYNAMIC,	"FEC_JDE", { new CCFODynamic(nil, nil, nil, DetectJoystickDraw, nil) }, 80, 200, MENUALIGN_LEFT,
		MENUACTION_GOBACK,		"FEDS_TB", {nil, SAVESLOT_NONE, MENUPAGE_NONE}, 320, 225, MENUALIGN_CENTER,
	},
#endif

		
#ifdef MISSION_REPLAY
	// MENUPAGE_MISSION_RETRY = 57 on mobile

	{ "M_FAIL", MENUPAGE_DISABLED, nil, nil,
		MENUACTION_LABEL,			"FESZ_RM",  { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, 0,
		MENUACTION_CHANGEMENU,		"FEM_YES",  { nil, SAVESLOT_NONE, MENUPAGE_LOADING_IN_PROGRESS }, 320, 200, MENUALIGN_CENTER,
		MENUACTION_REJECT_RETRY,	"FEM_NO",   { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 320, 225, MENUALIGN_CENTER,
	},
#endif

#ifdef NINTENDO_WII
	// One row per cheat.  A CFO dynamic with a nil variable and a non-nil button
	// handler is a button, and "Restore Default" further up this file is already
	// exactly that -- so no new menu action and no new option class was needed.
	//
	// Grouped rather than listed flat because a page holds NUM_MENUROWS (18) rows and
	// there are 29 cheats, and because "All Cars Heli" next to "Kangaroo" helps nobody.
	#define WII_CHEAT_ROW(label, fn) \
		MENUACTION_CFO_DYNAMIC, label, { new CCFODynamic(nil, nil, nil, nil, fn) }, 0, 0, MENUALIGN_CENTER,

	// MENUPAGE_CHEATS
	//
	// The layout is the one the game's own option pages use -- rows from 78 on a
	// 25 pixel line -- rather than the defaults: a full page then ends where a
	// native one does, instead of running down into the help box.  Every page
	// this port adds carries it.

	// Parent is the Enhancements page, not MENUPAGE_NONE: GetPreviousPageOption
	// resolves a NONE parent to the pause menu, so Back out of the cheats would
	// have skipped straight past the page that opened it.
	{ "WII_CHT", MENUPAGE_ENHANCEMENTS, new CCustomScreenLayout({320, 78, 25, false}), nil,
		MENUACTION_CHANGEMENU, "WII_CPL", { nil, SAVESLOT_NONE, MENUPAGE_CHEATS_PLAYER }, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU, "WII_CWH", { nil, SAVESLOT_NONE, MENUPAGE_CHEATS_WORLD }, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU, "WII_CVH", { nil, SAVESLOT_NONE, MENUPAGE_CHEATS_VEHICLES }, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU, "WII_CDB", { nil, SAVESLOT_NONE, MENUPAGE_CHEATS_DEBUG }, 0, 0, MENUALIGN_CENTER,
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, MENUALIGN_CENTER,
	},

	// MENUPAGE_CHEATS_PLAYER
	//
	// Grouped the way the game's own cheat list groups them: Player, World (weather,
	// clock and pedestrians), Vehicles, and the port's Debug toggles.  A few rows used
	// to sit wherever they were first added -- the pedestrians-carry-weapons code was
	// under Player, and the handling change under nothing vehicle-shaped at all.

	{ "WII_CPL", MENUPAGE_CHEATS, new CCustomScreenLayout({320, 78, 25, false}), nil,
		WII_CHEAT_ROW("WII_HLT", WiiCheat_Health)
		WII_CHEAT_ROW("WII_ARO", WiiCheat_Armour)
		WII_CHEAT_ROW("WII_MON", WiiCheat_Money)
		WII_CHEAT_ROW("WII_WPN", WiiCheat_WeaponCheat1)
		WII_CHEAT_ROW("WII_CHP", WiiCheat_ChangePlayer)
		WII_CHEAT_ROW("WII_KAN", WiiCheat_Kangaroo)
		WII_CHEAT_ROW("WII_WUU", WiiCheat_WantedLevelUp)
		WII_CHEAT_ROW("WII_WDN", WiiCheat_WantedLevelDown)
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, MENUALIGN_CENTER,
	},

	// MENUPAGE_CHEATS_WORLD

	{ "WII_CWH", MENUPAGE_CHEATS, new CCustomScreenLayout({320, 78, 25, false}), nil,
		WII_CHEAT_ROW("WII_SUN", WiiCheat_Sunny)
		WII_CHEAT_ROW("WII_CLD", WiiCheat_Cloudy)
		WII_CHEAT_ROW("WII_RAI", WiiCheat_Rainy)
		WII_CHEAT_ROW("WII_FOG", WiiCheat_Foggy)
		WII_CHEAT_ROW("WII_FWX", WiiCheat_FastWeather)
		WII_CHEAT_ROW("WII_FTM", WiiCheat_FastTime)
		WII_CHEAT_ROW("WII_STM", WiiCheat_SlowTime)
		WII_CHEAT_ROW("WII_MAY", WiiCheat_Mayhem)
		WII_CHEAT_ROW("WII_ATK", WiiCheat_EverybodyAttacksPlayer)
		WII_CHEAT_ROW("WII_AWP", WiiCheat_WeaponsForAll)
		WII_CHEAT_ROW("WII_BUP", WiiCheat_BlowUpCars)
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, MENUALIGN_CENTER,
	},

	// MENUPAGE_CHEATS_VEHICLES

	{ "WII_CVH", MENUPAGE_CHEATS, new CCustomScreenLayout({320, 78, 25, false}), nil,
		WII_CHEAT_ROW("WII_RHI", WiiCheat_Rhino)
		WII_CHEAT_ROW("WII_BLD", WiiCheat_Bloodra)
		WII_CHEAT_ROW("WII_ROM", WiiCheat_Romero)
		WII_CHEAT_ROW("WII_LOV", WiiCheat_Lovefist)
		WII_CHEAT_ROW("WII_TRS", WiiCheat_Trash)
		WII_CHEAT_ROW("WII_BLB", WiiCheat_Bloodrb)
		WII_CHEAT_ROW("WII_SBT", WiiCheat_Sabretur)
		WII_CHEAT_ROW("WII_CAD", WiiCheat_Caddy)
		WII_CHEAT_ROW("WII_HTA", WiiCheat_Hotrina)
		WII_CHEAT_ROW("WII_HTB", WiiCheat_Hotrinb)
		WII_CHEAT_ROW("WII_CCB", WiiCheat_ChittyChittyBangBang)
		WII_CHEAT_ROW("WII_GRP", WiiCheat_StrongGrip)
		WII_CHEAT_ROW("WII_WCL", WiiCheat_WallClimbing)
		WII_CHEAT_ROW("WII_HEL", WiiCheat_AllCarsHeli)
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, MENUALIGN_CENTER,
	},

	// MENUPAGE_CHEATS_DEBUG
	//
	// The render and debug toggles the original also exposed as button sequences.  They
	// are here rather than hidden because on this port they are the only way to reach
	// anything like a debug view at all.

	{ "WII_CDB", MENUPAGE_CHEATS, new CCustomScreenLayout({320, 78, 25, false}), nil,
		WII_CHEAT_ROW("WII_WHL", WiiCheat_OnlyRenderWheels)
		WII_CHEAT_ROW("WII_NSB", WiiCheat_NoSeaBed)
		WII_CHEAT_ROW("WII_WLY", WiiCheat_RenderWaterLayers)
		WII_CHEAT_ROW("WII_DBG", WiiCheat_SwitchDebugDisplay)
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, MENUALIGN_CENTER,
	},

	#undef WII_CHEAT_ROW

	// MENUPAGE_ENHANCEMENTS
	//
	// Everything this port added, in one place off the pause menu: the cheats menu,
	// couch co-op, and the options that used to be scattered over the Mouse/IR page
	// and the pause page.  Co-op is here rather than on the pause page itself because
	// it is a mode, not a destination, and because it is one of these.

	{ "WII_ENH", MENUPAGE_NONE, new CCustomScreenLayout({320, 78, 25, false}), nil,
		MENUACTION_CHANGEMENU,	"WII_CHE",	{ nil, SAVESLOT_NONE, MENUPAGE_CHEATS }, 0, 0, MENUALIGN_CENTER,
		MENUACTION_CHANGEMENU,	"WII_COP",	{ nil, SAVESLOT_NONE, MENUPAGE_COOP }, 0, 0, MENUALIGN_CENTER,
		DRIVEBY_WEAPONS_TOGGLE
		AIM_ASSIST_TOGGLE
		POINTER_AIM_TOGGLE
		POINTER_BOX_SELECT
		POINTER_CAR_TOGGLE
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, MENUALIGN_CENTER,
	},

	// MENUPAGE_COOP
	//
	// Couch co-op's own settings.  Its own page rather than more rows on the
	// Enhancements page: co-op is a mode with settings of its own, and
	// Enhancements was becoming the place everything went.

	{ "WII_COP", MENUPAGE_ENHANCEMENTS, new CCustomScreenLayout({320, 78, 25, false}), nil,
		COUCH_COOP_TOGGLE
		COOP_SKIN_SELECT
		FRIENDLY_FIRE_TOGGLE
		SHARED_WANTED_TOGGLE
		MENUACTION_GOBACK,		"FEDS_TB", { nil, SAVESLOT_NONE, MENUPAGE_NONE }, 0, 0, MENUALIGN_CENTER,
	},
#endif

	// MENUPAGE_OUTRO = 34
	{ "", 0, nil, nil, },
};

#endif
#endif
