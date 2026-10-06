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
// 0 small (the default), 1 medium, 2 large.  Persisted to the INI.
extern int8_t WiiPointerBox;

// Whether the pointer aims while driving.  Off by default because it does not work
// yet, and shipping a broken behaviour as the default is worse than not having it:
// this is a switch to try it on and off in one sitting, not a promise that it works.
extern int8_t WiiAimInCar;

#endif
