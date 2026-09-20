#pragma once

#ifdef NINTENDO_WII

// Replaces the stdout devoptab writer with a silent one.  On the Wii the
// console device draws every printf byte onto the game's framebuffer, which
// was scrolling the menus and the game image around; upstream reads fine on
// PC because its console is a real text window.  The hook swallows the bytes
// and does nothing else: debug.log has its own small set of writers
// (wiiLog/WiiTraceReport), so the engine's printf-family lines (Replay,
// Restart, Script*, mission audio and the like) reach neither medium like
// they used to on the original PC build.
//
// Called once, from the Wii boot path before anything prints.  The five early
// boot banners print through the console before this is installed and land on
// the television at boot, where there is nothing to scroll yet.
void WiiStdoutHookInstall(void);

#endif // NINTENDO_WII
