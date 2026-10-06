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

// Whether the pointer aims while driving.  With it on, vehicles are followed by the
// SA-style camera (CCam::Process_FollowCar_SA) instead of Vice City's own, because
// that is the only car camera that reads the pointer: the view swings when the
// pointer is pushed past the aim box, the Rhino's turret and the fire truck's hose
// follow it, and a drive-by goes out of whichever side the view is turned to and at
// the crosshair.  Off gives the stock car camera back.
extern int8_t WiiAimInCar;

// The "Drive-By Weapons" row on the Mouse/IR page, persisted to the INI.  Vice City
// only lets the driver fire a submachine gun out of a side window.  With this on, the
// handguns, shotguns, rifles and sniper rifles can be used from a car too, the way the
// games after Vice City allow, each at its own rate of fire and with its own reload.
// Melee, thrown and heavy weapons stay out.  Off is stock Vice City.  See
// WiiDriveByWeaponAllowed in WeaponInfo.h and WiiDriveByPaceShot in Weapon.h.
extern int8_t WiiDriveByAnyWeapon;

// True while that is in force: the Standard method with Pointer Aim and Aim In Car all
// on, and no GameCube pad plugged in (one of those silences the pointer).  The engine
// asks this in the few places a vehicle has to behave differently for it.
bool WiiPointerAimInCar(void);

#endif
