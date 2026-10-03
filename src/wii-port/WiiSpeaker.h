#ifndef WIISPEAKER_H
#define WIISPEAKER_H

#include <stdint.h>

// The radio-tuning static (SOUND_RADIO_CHANGE) carried through the Wiimote's own
// speaker as well, so the TV and the remote change station together.
//
// Free of libogc headers on purpose: the frontend and the music manager include
// this, and neither can take gccore.h.

// The "Remote Speaker" row on the sound page, persisted to the INI.  0 mutes it.
extern int8_t WiiRemoteSpeakerEnabled;

// Asks for one burst of static on the first remote.  Safe to call whenever; it is
// a no-op with the option off or no remote connected.
void WiiSpeakerPlayTuneStatic(void);

// Advances the speaker's power/stream state.  Called once per frame from
// WiiPadScan, after WPAD_ScanPads.
void WiiSpeakerService(void);

#endif
