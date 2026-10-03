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

#endif
