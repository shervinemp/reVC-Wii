#ifndef WIIPOINTERAIM_H
#define WIIPOINTERAIM_H

#include <stdint.h>

// Pointer aiming: with a gun out in the Standard control method, the Wiimote
// pointer moves the crosshair around the screen and the camera only turns when
// the pointer goes past the edge of a box around the middle (WiiPad.cpp).
//
// Free of libogc headers on purpose, like WiiSpeaker.h: the frontend includes it.

// The "Pointer Aim" row on the Mouse/IR controls page, persisted to the INI.
// 0 leaves the crosshair fixed and the pointer as a plain rate camera.
extern int8_t WiiPointerAimEnabled;

// How far from the middle the crosshair can roam before the camera turns instead:
// 0 small, 1 medium (the default), 2 large.  Persisted to the INI.
extern int8_t WiiPointerBox;

// Which shape the turn-rate ramp takes.  A player setting rather than a constant
// because the right answer is a matter of hand and hardware, not arithmetic: the
// width of the control band can be computed, but whether precision or mid-range
// speed matters more cannot, and guessing has already been wrong once.
extern int8_t WiiAimCurve;

#endif
