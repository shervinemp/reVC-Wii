#ifndef WIISPEAKER_H
#define WIISPEAKER_H

#include <stdint.h>

// Short sounds carried through the Wiimote's own speaker as well as the TV, the
// way Wii games do it: the radio-tuning static (SOUND_RADIO_CHANGE), the player's
// gunshots and a ringing payphone.
//
// Free of libogc headers on purpose: the frontend, the music manager and the game
// code include this, and none of them can take gccore.h.

// The "Remote Speaker" row on the sound page, persisted to the INI.  0 mutes all of it.
extern int8_t WiiRemoteSpeakerEnabled;

// Each asks for one clip on the first remote.  Safe to call whenever; they are no-ops
// with the option off or no remote connected, and a request that has waited too long
// behind another clip (or the speaker powering up) is dropped rather than played late.
void WiiSpeakerPlayTuneStatic(void);
void WiiSpeakerPlayShot(void);
void WiiSpeakerPlayRing(void);

// Advances the speaker's power/stream state.  Called once per frame from
// WiiPadScan, after WPAD_ScanPads.
void WiiSpeakerService(void);

#endif
