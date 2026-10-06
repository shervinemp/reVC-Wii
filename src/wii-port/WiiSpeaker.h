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

// Phone calls: a line of mission speech played through the remote, as if it were held
// to the ear.  The game's streamed-audio code drives these.
//
// How the TV treats the line while it is on the remote: 0 off (the TV plays it as
// always), 1 remote only (the TV is muted), 2 both (the TV stays at full volume, so
// nothing is lost if the remote cannot be heard).  The "Phone Calls" row on the sound
// page; persisted to the INI.
extern int8_t WiiPhoneRemoteMode;

// Brings the speaker up ahead of a call (when the phone is picked up), because a line
// can only be routed to a speaker that is already on.
void WiiSpeakerWake(void);

// Starts routing a line of the given length and sample rate.  False means the TV plays
// it as usual: the option is off, there is no remote, the speaker is not up yet, or
// something else is still streaming.
bool WiiSpeakerBeginCall(uint32_t lengthMs, uint32_t sampleRate);

// Interleaved 16-bit samples in native byte order, as they are queued to the TV.
void WiiSpeakerFeedCall(const int16_t *pcm, uint32_t frames, uint32_t channels);

// The line finished or was stopped.
void WiiSpeakerEndCall(void);

// The TV stream was paused: the remote cannot follow, so the line goes back to the TV.
void WiiSpeakerAbortCall(void);

// True while the line is on the remote.  The stream checks it every frame, so a remote
// that is switched off or lost hands the line back to the TV.
bool WiiSpeakerCallActive(void);

// The share of normal volume the TV should play a routed line at.
uint32_t WiiSpeakerCallTvPercent(void);

// Advances the speaker's power/stream state.  Called once per frame from
// WiiPadScan, after WPAD_ScanPads.
void WiiSpeakerService(void);

#endif
